#include "RecorderManager.hpp"
#include "AudioCapture.hpp"
#include "MicrophoneCapture.hpp"

#include <Geode/cocos/platform/CCGL.h>
#include <Geode/utils/async.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

using namespace geode::prelude;

RecorderManager& RecorderManager::get() {
    static RecorderManager instance;
    return instance;
}

RecorderManager::~RecorderManager() {
    if (m_sessionActive.load(std::memory_order_acquire)) {
        (void)stop();
    }
}

std::string RecorderManager::sanitizeExtension(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isalnum(c);
    }), value.end());
    if (value.empty()) value = "mp4";
    return value;
}

std::string RecorderManager::timestampForFilename() {
    auto now = std::time(nullptr);
    std::tm local {};
#ifdef GEODE_IS_WINDOWS
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d_%H-%M-%S");
    return stream.str();
}

Result<> RecorderManager::start() {
    if (m_sessionActive.exchange(true, std::memory_order_acq_rel)) {
        return Err("A recording is already active.");
    }

    auto failStart = [this](std::string const& message) -> Result<> {
        m_recording.store(false, std::memory_order_release);
        m_sessionActive.store(false, std::memory_order_release);
        return Err(message);
    };

    auto director = CCDirector::get();
    auto view = director ? director->getOpenGLView() : nullptr;
    if (!view) return failStart("Geometry Dash has no active OpenGL view.");

    GLint viewport[4] {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        return failStart("OpenGL returned an invalid framebuffer size.");
    }

    m_width = static_cast<std::uint32_t>(viewport[2]);
    m_height = static_cast<std::uint32_t>(viewport[3]);
    // Set the pack alignment once before capture; avoiding a state call on
    // every render tick reduces driver overhead on the gameplay thread.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    auto fpsSetting = Mod::get()->getSettingValue<int64_t>("fps");
    auto bitrateMbps = Mod::get()->getSettingValue<int64_t>("bitrate-mbps");
    auto queueDepth = Mod::get()->getSettingValue<int64_t>("queue-depth");
    auto codec = Mod::get()->getSettingValue<std::string>("codec");
    auto extension = sanitizeExtension(Mod::get()->getSettingValue<std::string>("container"));

    auto fps = static_cast<std::uint16_t>(std::clamp<int64_t>(fpsSetting, 1, 1000));
    auto bitrate = std::clamp<int64_t>(bitrateMbps, 1, 1000) * 1'000'000;
    m_queueCapacity = static_cast<std::size_t>(std::clamp<int64_t>(queueDepth, 2, 8));
    m_fps = fps;

    auto recordingsDirectory = Mod::get()->getSaveDir() / "recordings";
    std::error_code filesystemError;
    std::filesystem::create_directories(recordingsDirectory, filesystemError);
    if (filesystemError) {
        return failStart(fmt::format("Unable to create recordings directory: {}", filesystemError.message()));
    }

    m_videoPath = recordingsDirectory / ("GD_" + timestampForFilename() + "." + extension);
    m_finalPath = m_videoPath;

    ffmpeg::RenderSettings settings;
    // Use the API's matching device type for hardware encoders. Software
    // codecs stay on NONE; hardware encoders otherwise fall back to their
    // normal FFmpeg path only when the selected device is available.
    settings.m_hardwareAccelerationType = ffmpeg::HardwareAccelerationType::NONE;
    if (codec == "h264_nvenc" || codec == "hevc_nvenc") {
        settings.m_hardwareAccelerationType = ffmpeg::HardwareAccelerationType::CUDA;
    } else if (codec == "h264_amf" || codec == "hevc_amf") {
        settings.m_hardwareAccelerationType = ffmpeg::HardwareAccelerationType::D3D11VA;
    }
    settings.m_pixelFormat = ffmpeg::PixelFormat::RGBA;
    settings.m_codec = codec;
    settings.m_doVerticalFlip = true;
    settings.m_bitrate = bitrate;
    settings.m_width = m_width;
    settings.m_height = m_height;
    settings.m_fps = fps;
    settings.m_outputFile = m_videoPath;

    auto recorder = std::make_unique<ffmpeg::events::Recorder>();
    if (!recorder->isValid()) {
        return failStart("Eclipse FFmpeg API is installed but its recorder interface is unavailable.");
    }

    auto initialized = recorder->init(settings);
    if (initialized.isErr()) {
        return failStart(initialized.unwrapErr());
    }

    auto frameBytes = static_cast<std::size_t>(m_width) * m_height * 4u;
    try {
        std::scoped_lock queueLock(m_queueMutex);
        m_pendingFrames.clear();
        m_freeFrames.clear();
        m_freeFrames.reserve(m_queueCapacity);
        for (std::size_t index = 0; index < m_queueCapacity; ++index) {
            m_freeFrames.push_back(std::make_shared<std::vector<std::uint8_t>>(frameBytes, 0u));
        }
        m_lastFrame.reset();
        m_stopRequested = false;
    } catch (std::bad_alloc const&) {
        recorder->stop();
        return failStart("Not enough memory for the asynchronous frame queue.");
    }

    {
        std::scoped_lock stateLock(m_stateMutex);
        m_encoderError.clear();
        m_recorder = std::move(recorder);
    }

    m_framePeriod = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(fps))
    );
    m_nextFrame = std::chrono::steady_clock::now();
    m_framesCaptured.store(0, std::memory_order_relaxed);
    m_framesDue.store(0, std::memory_order_relaxed);
    m_framesWritten.store(0, std::memory_order_relaxed);
    m_framesDropped.store(0, std::memory_order_relaxed);
    m_recording.store(true, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    auto captureAudio = Mod::get()->getSettingValue<bool>("capture-game-audio");
    auto audioOverride = Mod::get()->getSettingValue<std::string>("audio-file");
    if (captureAudio && audioOverride.empty()) {
        auto installed = AudioCapture::get().ensureInstalled();
        if (installed.isErr()) {
            // Audio capture failing shouldn't stop the video recording;
            // just log it and continue without game audio.
            log::warn("Audio capture unavailable: {}", installed.unwrapErr());
        } else {
            AudioCapture::get().beginCapture();
        }
    }
    if (Mod::get()->getSettingValue<bool>("capture-microphone") && audioOverride.empty()) {
        auto microphone = MicrophoneCapture::get().start();
        if (microphone.isErr()) {
            log::warn("Microphone capture unavailable: {}", microphone.unwrapErr());
        }
    }

    try {
        m_encoderThread = std::thread(&RecorderManager::encoderLoop, this);
    } catch (std::system_error const& error) {
        m_recording.store(false, std::memory_order_release);
        AudioCapture::get().endCapture();
        MicrophoneCapture::get().stop();
        {
            std::scoped_lock stateLock(m_stateMutex);
            m_recorder->stop();
            m_recorder.reset();
        }
        return failStart(fmt::format("Unable to create encoder thread: {}", error.what()));
    }

    log::info(
        "Recording started asynchronously: {}x{} @ {} FPS, queue {} -> {}",
        m_width, m_height, fps, m_queueCapacity, m_videoPath.string()
    );
    return Ok();
}

void RecorderManager::advanceFrameClock(std::chrono::steady_clock::time_point now) {
    do {
        m_nextFrame += m_framePeriod;
    } while (m_nextFrame <= now);
}

geode::Result<> RecorderManager::pause() {
    if (!m_recording.load(std::memory_order_acquire)) {
        return Err("No recording is active.");
    }
    if (m_paused.exchange(true, std::memory_order_acq_rel)) {
        return Ok();
    }
    AudioCapture::get().endCapture();
    MicrophoneCapture::get().setAccepting(false);
    return Ok();
}

geode::Result<> RecorderManager::resume() {
    if (!m_recording.load(std::memory_order_acquire)) {
        return Err("No recording is active.");
    }
    if (!m_paused.exchange(false, std::memory_order_acq_rel)) {
        return Ok();
    }

    // Do not let time spent paused turn into a burst of synthetic frames.
    m_nextFrame = std::chrono::steady_clock::now();
    auto captureAudio = Mod::get()->getSettingValue<bool>("capture-game-audio");
    auto audioOverride = Mod::get()->getSettingValue<std::string>("audio-file");
    if (captureAudio && audioOverride.empty()) {
        AudioCapture::get().beginCapture(false);
    }
    if (Mod::get()->getSettingValue<bool>("capture-microphone") && audioOverride.empty()) {
        MicrophoneCapture::get().setAccepting(true);
    }
    return Ok();
}

Result<> RecorderManager::captureFrame() {
    if (!m_recording.load(std::memory_order_acquire)) {
        auto error = encoderError();
        if (error.empty()) {
            return Ok();
        }
        return Err(error);
    }
    if (m_paused.load(std::memory_order_acquire)) return Ok();

    auto now = std::chrono::steady_clock::now();
    if (now < m_nextFrame) return Ok();

    // Account for every output tick that elapsed, including ticks missed by
    // a render hitch. Only one fresh readback is needed; the encoder will
    // duplicate the last completed frame for the other ticks.
    auto elapsed = now - m_nextFrame;
    auto elapsedTicks = static_cast<std::uint64_t>(
        elapsed / m_framePeriod
    ) + 1;
    m_nextFrame += m_framePeriod * static_cast<std::int64_t>(elapsedTicks);
    m_framesDue.fetch_add(elapsedTicks, std::memory_order_relaxed);

    GLint viewport[4] {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] != static_cast<GLint>(m_width) || viewport[3] != static_cast<GLint>(m_height)) {
        return Err("Framebuffer size changed during recording; stop and begin a new recording.");
    }

    FrameBuffer frame;
    {
        std::scoped_lock queueLock(m_queueMutex);
        if (m_freeFrames.empty()) {
            m_framesDropped.fetch_add(elapsedTicks, std::memory_order_relaxed);
            // The encoder can't keep up, so there's no free buffer to grab a
            // fresh capture into this tick. We don't skip the tick outright
            // (that would silently shorten the encoded video relative to how
            // long the recording actually ran, speeding up video and, in
            // turn, desyncing/pitching the audio mux) - instead the encoder
            // thread re-writes its last frame to cover this tick once it's
            // done with its current backlog (see encoderLoop()). Wake it in
            // case it's already idle. This is O(1): no frame data is copied
            // on the render thread.
            m_queueCondition.notify_one();
            return Ok();
        }
        frame = std::move(m_freeFrames.back());
        m_freeFrames.pop_back();
    }

    glReadPixels(
        0, 0,
        static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height),
        GL_RGBA, GL_UNSIGNED_BYTE, frame->data()
    );
    auto glError = glGetError();
    if (glError != GL_NO_ERROR) {
        std::scoped_lock queueLock(m_queueMutex);
        m_freeFrames.push_back(std::move(frame));
        return Err("glReadPixels failed with OpenGL error {}.", static_cast<unsigned int>(glError));
    }

    {
        std::scoped_lock queueLock(m_queueMutex);
        if (!m_recording.load(std::memory_order_acquire) || m_stopRequested) {
            m_freeFrames.push_back(std::move(frame));
            return Ok();
        }
        m_pendingFrames.push_back(std::move(frame));
    }
    m_framesCaptured.fetch_add(1, std::memory_order_relaxed);
    if (elapsedTicks > 1) {
        m_framesDropped.fetch_add(elapsedTicks - 1, std::memory_order_relaxed);
    }
    m_queueCondition.notify_one();
    return Ok();
}

void RecorderManager::encoderLoop() {
    for (;;) {
        FrameBuffer frame;
        bool isPadding = false;
        {
            std::unique_lock queueLock(m_queueMutex);
            m_queueCondition.wait(queueLock, [this] {
                if (!m_pendingFrames.empty()) return true;
                // Nothing new captured. Are we behind where real elapsed
                // time says the video should be? If so and we have a frame
                // to duplicate, there's padding work to do (or, if stopping,
                // this is what lets stop drain fully before exiting).
                auto target = m_framesDue.load(std::memory_order_relaxed);
                if (m_lastFrame && m_framesWritten.load(std::memory_order_relaxed) < target) {
                    return true;
                }
                return m_stopRequested;
            });

            if (!m_pendingFrames.empty()) {
                frame = std::move(m_pendingFrames.front());
                m_pendingFrames.pop_front();
            } else {
                auto target = m_framesDue.load(std::memory_order_relaxed);
                if (m_lastFrame && m_framesWritten.load(std::memory_order_relaxed) < target) {
                    frame = m_lastFrame; // shared_ptr copy - cheap, no pixel data touched
                    isPadding = true;
                } else if (m_stopRequested) {
                    break;
                } else {
                    continue;
                }
            }
        }

        Result<> written = Err("Recorder disappeared while encoding.");
        {
            std::scoped_lock stateLock(m_stateMutex);
            if (m_recorder) written = m_recorder->writeFrame(*frame);
        }

        {
            std::scoped_lock queueLock(m_queueMutex);
            if (isPadding) {
                // frame *is* m_lastFrame; it's still reserved for that role,
                // nothing to recycle.
            } else {
                // This frame supersedes whatever was previously the "last
                // frame". Recycle the old one back into the free pool now
                // that nothing can reference it anymore.
                if (m_lastFrame) {
                    m_freeFrames.push_back(m_lastFrame);
                }
                m_lastFrame = frame;
            }
        }

        if (written.isErr()) {
            setEncoderError(written.unwrapErr());
            m_recording.store(false, std::memory_order_release);
            std::scoped_lock queueLock(m_queueMutex);
            m_stopRequested = true;
            m_pendingFrames.clear();
            break;
        }

        m_framesWritten.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecorderManager::setEncoderError(std::string message) {
    std::scoped_lock stateLock(m_stateMutex);
    m_encoderError = std::move(message);
}

std::string RecorderManager::encoderError() const {
    std::scoped_lock stateLock(m_stateMutex);
    return m_encoderError;
}

void RecorderManager::stopAsync(std::function<void(Result<std::filesystem::path>)> callback) {
    if (m_stopping.exchange(true, std::memory_order_acq_rel)) {
        // Already stopping; don't spawn a second background stop().
        return;
    }

    // stop() joins the encoder thread and does blocking FFmpeg audio-mux
    // I/O, so it has to run on Arc's blocking thread pool rather than
    // inside the regular async runtime (see tutorials/async.md, "Blocking").
    // The result is then handed back to the caller on the main thread via
    // waitForMainThread, since it will be touching Cocos/UI.
    async::spawn([this, callback = std::move(callback)]() -> arc::Future<> {
        auto handle = async::runtime().spawnBlocking<Result<std::filesystem::path>>([this] {
            return this->stop();
        });
        auto result = co_await handle;
        co_await async::waitForMainThread<void>([this, callback = std::move(callback), result = std::move(result)] {
            m_stopping.store(false, std::memory_order_release);
            callback(result);
        });
        co_return;
    });
}

bool RecorderManager::isStopping() const {
    return m_stopping.load(std::memory_order_acquire);
}

Result<std::filesystem::path> RecorderManager::stop() {
    if (!m_sessionActive.load(std::memory_order_acquire)) {
        return Err("No recording is active.");
    }

    m_recording.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
    AudioCapture::get().endCapture();
    MicrophoneCapture::get().stop();
    {
        std::scoped_lock queueLock(m_queueMutex);
        m_stopRequested = true;
    }
    m_queueCondition.notify_all();

    if (m_encoderThread.joinable()) {
        m_encoderThread.join();
    }

    std::filesystem::path videoPath;
    {
        std::scoped_lock stateLock(m_stateMutex);
        if (m_recorder) {
            m_recorder->stop();
            m_recorder.reset();
        }
        videoPath = m_videoPath;
    }

    {
        std::scoped_lock queueLock(m_queueMutex);
        m_pendingFrames.clear();
        m_freeFrames.clear();
        m_lastFrame.reset();
        m_stopRequested = false;
    }

    m_sessionActive.store(false, std::memory_order_release);

    auto error = encoderError();
    if (!error.empty()) {
        return Err("Encoding failed: {}. A partial video may exist at {}", error, videoPath.string());
    }

    auto mixed = finishAudioMux(videoPath);
    if (mixed.isErr()) {
        log::warn("Video was saved, but audio muxing failed: {}", mixed.unwrapErr());
        return Ok(videoPath);
    }

    std::filesystem::path finalPath;
    {
        std::scoped_lock stateLock(m_stateMutex);
        finalPath = m_finalPath;
    }
    log::info(
        "Recording stopped: {} encoded, {} dropped -> {}",
        m_framesWritten.load(), m_framesDropped.load(), finalPath.string()
    );
    return Ok(finalPath);
}

Result<> RecorderManager::finishAudioMux(std::filesystem::path const& videoPath) {
    auto audioValue = Mod::get()->getSettingValue<std::string>("audio-file");

    // An explicit audio file override always takes priority over captured
    // game audio.
    if (!audioValue.empty()) {
        std::filesystem::path audioPath(audioValue);
        if (!std::filesystem::exists(audioPath)) {
            return Err("Configured audio file does not exist: {}", audioPath.string());
        }

        auto mixedPath = videoPath.parent_path() / (videoPath.stem().string() + "_audio.mp4");
        auto result = ffmpeg::events::AudioMixer::mixVideoAudio(videoPath, audioPath, mixedPath);
        if (result.isErr()) {
            std::error_code cleanupError;
            std::filesystem::remove(mixedPath, cleanupError);
            return Err(result.unwrapErr());
        }

        std::error_code removeError;
        std::filesystem::remove(videoPath, removeError);
        std::scoped_lock stateLock(m_stateMutex);
        m_finalPath = mixedPath;
        return Ok();
    }

    auto captureAudio = Mod::get()->getSettingValue<bool>("capture-game-audio");
    auto captureMicrophone = Mod::get()->getSettingValue<bool>("capture-microphone");
    auto gameSamples = captureAudio ? AudioCapture::get().takeSamples() : std::vector<float>();
    auto microphoneSamples = captureMicrophone ? MicrophoneCapture::get().takeSamples() : std::vector<float>();
    if (gameSamples.empty() && microphoneSamples.empty()) {
        std::scoped_lock stateLock(m_stateMutex);
        m_finalPath = videoPath;
        return Ok();
    }

    auto gameRate = AudioCapture::get().sampleRate();
    if (gameRate == 0) gameRate = 44100;
    auto microphoneRate = MicrophoneCapture::get().sampleRate();
    if (microphoneRate == 0) microphoneRate = 48000;

    // The documented raw-audio mixer infers the source rate from the sample
    // count and video duration. Normalize the captured segment to exactly the
    // video duration first; it then resamples from the real FMOD rate without
    // changing pitch, and no temporary WAV is needed.
    auto expectedFrames = static_cast<std::uint64_t>(std::llround(
        static_cast<double>(m_framesWritten.load(std::memory_order_relaxed))
        / static_cast<double>(m_fps) * std::max<std::uint32_t>(gameRate, microphoneRate)
    ));
    auto expectedValues = expectedFrames * 2u;
    auto normalizeStereo = [&](std::vector<float> const& input, std::uint32_t rate) {
        std::vector<float> output(expectedValues, 0.f);
        if (input.empty()) return output;
        auto inputFrames = input.size() / 2u;
        if (inputFrames == 0) return output;
        auto outputFrames = expectedFrames;
        for (std::uint64_t frame = 0; frame < outputFrames; ++frame) {
            auto source = std::min<std::uint64_t>(
                inputFrames - 1,
                static_cast<std::uint64_t>(static_cast<double>(frame) * rate / std::max<std::uint32_t>(gameRate, microphoneRate))
            );
            output[frame * 2] = input[source * 2];
            output[frame * 2 + 1] = input[source * 2 + 1];
        }
        return output;
    };
    auto normalizeMono = [&](std::vector<float> const& input, std::uint32_t rate) {
        std::vector<float> output(expectedFrames, 0.f);
        if (input.empty()) return output;
        for (std::uint64_t frame = 0; frame < expectedFrames; ++frame) {
            auto source = std::min<std::uint64_t>(
                input.size() - 1,
                static_cast<std::uint64_t>(static_cast<double>(frame) * rate / std::max<std::uint32_t>(gameRate, microphoneRate))
            );
            output[frame] = input[source];
        }
        return output;
    };
    auto game = normalizeStereo(gameSamples, gameRate);
    auto microphone = normalizeMono(microphoneSamples, microphoneRate);
    std::vector<float> samples(expectedValues, 0.f);
    for (std::uint64_t frame = 0; frame < expectedFrames; ++frame) {
        auto gameLeft = game[frame * 2];
        auto gameRight = game[frame * 2 + 1];
        auto mic = microphone[frame];
        if (!gameSamples.empty() && !microphoneSamples.empty()) {
            // Average both sources to avoid clipping when the game and mic
            // are simultaneously loud; this does not change either source's
            // speed or pitch.
            samples[frame * 2] = (gameLeft + mic) * .5f;
            samples[frame * 2 + 1] = (gameRight + mic) * .5f;
        } else if (!gameSamples.empty()) {
            samples[frame * 2] = gameLeft;
            samples[frame * 2 + 1] = gameRight;
        } else {
            samples[frame * 2] = mic;
            samples[frame * 2 + 1] = mic;
        }
    }

    auto mixedPath = videoPath.parent_path() / (videoPath.stem().string() + "_muxed.mp4");
    auto result = ffmpeg::events::AudioMixer::mixVideoRaw(videoPath, samples, mixedPath);
    std::error_code removeError;
    if (result.isErr()) {
        // Keep the silent video rather than losing the recording entirely.
        log::warn("Muxing captured game audio failed: {}", result.unwrapErr());
        std::filesystem::remove(mixedPath, removeError);
        std::scoped_lock stateLock(m_stateMutex);
        m_finalPath = videoPath;
        return Ok();
    }

    // Keep one user-facing recording only. The documented audio mixer always
    // creates an MP4, so use the original path only when it was already MP4;
    // otherwise retain the mixer-produced MP4 rather than giving it a false
    // extension.
    auto finalPath = videoPath.extension() == ".mp4" ? videoPath : mixedPath;
    if (finalPath == videoPath) {
        std::filesystem::remove(videoPath, removeError);
        std::filesystem::rename(mixedPath, videoPath, removeError);
        if (removeError) {
            log::warn("Audio was muxed, but replacing the silent video failed: {}", removeError.message());
            std::scoped_lock stateLock(m_stateMutex);
            m_finalPath = mixedPath;
            return Ok();
        }
    } else {
        std::filesystem::remove(videoPath, removeError);
    }

    std::scoped_lock stateLock(m_stateMutex);
    m_finalPath = finalPath;
    return Ok();
}

RecorderManager::Status RecorderManager::status() const {
    Status s;
    s.recording = m_recording.load(std::memory_order_acquire);
    s.framesWritten = m_framesWritten.load(std::memory_order_relaxed);
    s.framesDropped = m_framesDropped.load(std::memory_order_relaxed);
    return s;
}

bool RecorderManager::isRecording() const {
    return m_recording.load(std::memory_order_acquire);
}

bool RecorderManager::isPaused() const {
    return m_paused.load(std::memory_order_acquire);
}

std::filesystem::path RecorderManager::outputPath() const {
    std::scoped_lock stateLock(m_stateMutex);
    return m_finalPath;
}

std::string RecorderManager::statusText() const {
    if (!m_recording.load(std::memory_order_acquire)) return "";
    auto written = m_framesWritten.load(std::memory_order_relaxed);
    auto dropped = m_framesDropped.load(std::memory_order_relaxed);
    return dropped == 0
        ? fmt::format("REC  {}", written)
        : fmt::format("REC  {}  DROP {}", written, dropped);
}
