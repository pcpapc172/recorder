#pragma once

#include <Geode/Geode.hpp>
#include <eclipse.ffmpeg-api/include/events.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RecorderManager final {
public:
    static RecorderManager& get();

    geode::Result<> start();
    geode::Result<std::filesystem::path> stop();
    geode::Result<> captureFrame();
    geode::Result<> pause();
    geode::Result<> resume();

    // Runs stop() on Arc's blocking thread pool (encoder draining + audio
    // muxing both do blocking file/FFmpeg I/O that can take a while for
    // long recordings - see tutorials/async.md, "Blocking") and delivers
    // the result back to the callback on the main thread, so the caller
    // can show a "Saving..." popup instead of freezing the game for the
    // duration.
    // isStopping() reports true from the moment this is called until the
    // callback has run.
    void stopAsync(std::function<void(geode::Result<std::filesystem::path>)> callback);
    bool isStopping() const;

    // Live status snapshot for the F8 menu popup.
    struct Status {
        bool recording = false;
        std::uint64_t framesWritten = 0;
        std::uint64_t framesDropped = 0;
    };
    Status status() const;

    bool isRecording() const;
    bool isPaused() const;
    std::filesystem::path outputPath() const;
    std::string statusText() const;

private:
    RecorderManager() = default;
    ~RecorderManager();
    RecorderManager(RecorderManager const&) = delete;
    RecorderManager& operator=(RecorderManager const&) = delete;

    static std::string sanitizeExtension(std::string value);
    static std::string timestampForFilename();
    geode::Result<> finishAudioMux(std::filesystem::path const& videoPath);
    void encoderLoop();
    void setEncoderError(std::string message);
    std::string encoderError() const;
    void advanceFrameClock(std::chrono::steady_clock::time_point now);

    mutable std::mutex m_stateMutex;
    std::unique_ptr<ffmpeg::events::Recorder> m_recorder;
    std::filesystem::path m_videoPath;
    std::filesystem::path m_finalPath;
    std::string m_encoderError;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    using FrameBuffer = std::shared_ptr<std::vector<std::uint8_t>>;
    struct PendingFrame {
        FrameBuffer frame;
        // Output ticks that elapsed before this fresh capture. The encoder
        // writes these using the latest available frame before advancing to
        // this one, preserving chronological frame pacing.
        std::uint64_t repeatsBefore = 0;
    };
    std::deque<PendingFrame> m_pendingFrames;
    std::vector<FrameBuffer> m_freeFrames;
    // The most recently written frame, kept out of m_freeFrames while it
    // holds this role. When the render thread can't capture a fresh frame
    // (free pool exhausted) or simply hasn't produced one yet by the time
    // the encoder is idle, the encoder thread re-writes this same buffer
    // (a cheap shared_ptr copy, not a pixel-data copy) instead of the
    // render thread skipping the tick outright. That keeps the encoded
    // video's duration in sync with real elapsed time without ever
    // touching frame pixel data outside of encoderLoop/captureFrame's own
    // glReadPixels call.
    FrameBuffer m_lastFrame;
    // Dropped ticks that occurred after the last queued fresh frame. They
    // are attached to the next fresh frame as repeatsBefore, or drained
    // using m_lastFrame when recording stops first.
    std::uint64_t m_pendingRepeatTicks = 0;
    std::thread m_encoderThread;
    bool m_stopRequested = false;
    std::size_t m_queueCapacity = 3;

    std::chrono::steady_clock::time_point m_nextFrame {};
    std::chrono::steady_clock::duration m_framePeriod {};
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint16_t m_fps = 60;

    std::atomic<std::uint64_t> m_framesCaptured { 0 };
    std::atomic<std::uint64_t> m_framesWritten { 0 };
    std::atomic<std::uint64_t> m_framesDropped { 0 };
    std::atomic<bool> m_recording { false };
    std::atomic<bool> m_paused { false };
    std::atomic<bool> m_sessionActive { false };
    std::atomic<bool> m_stopping { false };
};
