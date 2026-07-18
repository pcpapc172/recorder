#include "AudioCapture.hpp"

#include <Geode/binding/FMODAudioEngine.hpp>

#include <cstring>
#include <utility>

using namespace geode::prelude;

AudioCapture& AudioCapture::get() {
    static AudioCapture instance;
    return instance;
}

FMOD_RESULT F_CALLBACK AudioCapture::readCallback(
    FMOD_DSP_STATE* dspState, float* inbuffer, float* outbuffer,
    unsigned int length, int inchannels, int* outchannels
) {
    // Always pass audio through untouched; this DSP must never change what
    // the player actually hears.
    std::memcpy(outbuffer, inbuffer, static_cast<std::size_t>(length) * inchannels * sizeof(float));
    *outchannels = inchannels;

    auto& self = AudioCapture::get();

    if (dspState && dspState->functions && dspState->functions->getsamplerate) {
        int rate = 0;
        if (dspState->functions->getsamplerate(dspState, &rate) == FMOD_OK && rate > 0) {
            self.m_sampleRate.store(static_cast<std::uint32_t>(rate), std::memory_order_relaxed);
        }
    }

    if (!self.m_capturing.load(std::memory_order_acquire)) {
        return FMOD_OK;
    }

    // Never make FMOD's real-time audio thread wait for the recorder. If the
    // stop/mux path currently owns the buffer, simply skip this audio block;
    // a small audio gap is preferable to audible DSP-thread crackle.
    std::unique_lock lock(self.m_bufferMutex, std::try_to_lock);
    if (!lock.owns_lock()) return FMOD_OK;
    if (inchannels == 2) {
        auto count = static_cast<std::size_t>(length) * 2;
        if (self.m_buffer.size() + count > self.m_buffer.capacity()) return FMOD_OK;
        self.m_buffer.insert(
            self.m_buffer.end(), inbuffer, inbuffer + static_cast<std::size_t>(length) * 2
        );
    } else if (inchannels == 1) {
        // Duplicate mono to interleaved stereo so the buffer always matches
        // what ffmpeg::events::AudioMixer::mixVideoRaw expects.
        auto count = static_cast<std::size_t>(length) * 2;
        if (self.m_buffer.size() + count > self.m_buffer.capacity()) return FMOD_OK;
        self.m_buffer.reserve(self.m_buffer.size() + count);
        for (unsigned int i = 0; i < length; ++i) {
            self.m_buffer.push_back(inbuffer[i]);
            self.m_buffer.push_back(inbuffer[i]);
        }
    } else if (inchannels > 2) {
        // Take just the first two channels as left/right.
        auto count = static_cast<std::size_t>(length) * 2;
        if (self.m_buffer.size() + count > self.m_buffer.capacity()) return FMOD_OK;
        self.m_buffer.reserve(self.m_buffer.size() + count);
        for (unsigned int i = 0; i < length; ++i) {
            auto const* frame = inbuffer + static_cast<std::size_t>(i) * inchannels;
            self.m_buffer.push_back(frame[0]);
            self.m_buffer.push_back(frame[1]);
        }
    }
    // inchannels == 0 shouldn't happen; nothing to capture if it does.

    return FMOD_OK;
}

Result<> AudioCapture::ensureInstalled() {
    if (m_installed.load(std::memory_order_acquire)) {
        return Ok();
    }

    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) {
        return Err("FMOD audio engine is not available yet.");
    }

    FMOD::System* system = engine->m_system;

    FMOD::ChannelGroup* master = nullptr;
    if (system->getMasterChannelGroup(&master) != FMOD_OK || !master) {
        return Err("Could not get FMOD's master channel group.");
    }

    FMOD_DSP_DESCRIPTION description {};
    description.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    std::strncpy(description.name, "GDSRAudioTap", sizeof(description.name) - 1);
    description.version = 1;
    description.numinputbuffers = 1;
    description.numoutputbuffers = 1;
    description.read = &AudioCapture::readCallback;

    FMOD::DSP* dsp = nullptr;
    if (system->createDSP(&description, &dsp) != FMOD_OK || !dsp) {
        return Err("Failed to create the FMOD audio tap DSP.");
    }

    if (master->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, dsp) != FMOD_OK) {
        dsp->release();
        return Err("Failed to attach the FMOD audio tap to the master channel group.");
    }

    m_dsp = dsp;
    m_masterGroup = master;
    m_installed.store(true, std::memory_order_release);
    log::info("GDSRAudioTap installed on FMOD master channel group.");
    return Ok();
}

void AudioCapture::beginCapture(bool clearBuffer) {
    if (clearBuffer) {
        std::scoped_lock lock(m_bufferMutex);
        m_buffer.clear();
        // Reserve on the game thread, never from FMOD's callback. This covers
        // ordinary recordings without a real-time vector reallocation.
        if (m_buffer.capacity() < 16u * 1024u * 1024u) {
            m_buffer.reserve(16u * 1024u * 1024u);
        }
    }
    m_capturing.store(true, std::memory_order_release);
}

void AudioCapture::endCapture() {
    m_capturing.store(false, std::memory_order_release);
}

std::vector<float> AudioCapture::takeSamples() {
    std::scoped_lock lock(m_bufferMutex);
    return std::exchange(m_buffer, std::vector<float>());
}

std::uint32_t AudioCapture::sampleRate() const {
    return m_sampleRate.load(std::memory_order_relaxed);
}
