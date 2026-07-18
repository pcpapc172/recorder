#pragma once

// FMOD::System / FMOD::DSP etc. are already declared by <Geode/Bindings.hpp>
// (which Geode/Geode.hpp pulls in) via <fmod.hpp>. fmod_dsp.h additionally
// gives us FMOD_DSP_DESCRIPTION and the raw callback typedefs, which are not
// included by fmod.hpp itself.
#include <Geode/Geode.hpp>
#include <fmod_dsp.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// Taps Geometry Dash's FMOD master channel group with a pass-through DSP unit
// so the fully mixed game audio (music + every sound effect) can be captured
// while a recording is in progress, without altering what the player hears.
//
// The DSP is installed once (lazily, on the first recording) at
// FMOD_CHANNELCONTROL_DSP_HEAD on FMODAudioEngine::sharedEngine()->m_system's
// master channel group, which is where the channel group's local DSP chain
// receives the already-summed output of every channel feeding into it - i.e.
// the complete final mix.
class AudioCapture final {
public:
    static AudioCapture& get();

    // Creates and attaches the tap DSP if it hasn't been already.
    // Safe to call from the main thread only.
    geode::Result<> ensureInstalled();

    // Starts/stops accumulating samples from the DSP read callback.
    // The DSP itself always stays installed and always passes audio through
    // unmodified; these only toggle whether incoming blocks are also copied
    // into the capture buffer.
    // clearBuffer is true for a new recording and false when resuming a
    // paused recording so the separate audio segments remain contiguous.
    void beginCapture(bool clearBuffer = true);
    void endCapture();

    // Returns the captured samples so far as interleaved stereo float32
    // (matching what ffmpeg::events::AudioMixer::mixVideoRaw expects) and
    // clears the internal buffer.
    std::vector<float> takeSamples();

    std::uint32_t sampleRate() const;

private:
    AudioCapture() = default;
    AudioCapture(AudioCapture const&) = delete;
    AudioCapture& operator=(AudioCapture const&) = delete;

    static FMOD_RESULT F_CALLBACK readCallback(
        FMOD_DSP_STATE* dspState, float* inbuffer, float* outbuffer,
        unsigned int length, int inchannels, int* outchannels
    );

    std::atomic<bool> m_installed { false };
    std::atomic<bool> m_capturing { false };
    std::atomic<std::uint32_t> m_sampleRate { 0 };

    FMOD::DSP* m_dsp = nullptr;
    FMOD::ChannelGroup* m_masterGroup = nullptr;

    std::mutex m_bufferMutex;
    std::vector<float> m_buffer;
};
