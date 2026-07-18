#pragma once

#include <Geode/Geode.hpp>
#include <fmod.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// FMOD's documented recording-driver API, following the same recordSound /
// recordStart / getRecordPosition / Sound::lock flow used by Globed's
// AudioManager. The worker thread keeps all device polling and PCM copying off
// Geometry Dash's render thread.
class MicrophoneCapture final {
public:
    struct Device {
        int id = -1;
        std::string name;
        FMOD_GUID guid{};
        int sampleRate = 48000;
    };

    static MicrophoneCapture& get();

    static std::vector<Device> enumerateDevices();
    static std::string guidString(FMOD_GUID const& guid);
    static std::string selectedDeviceName();
    static void cycleDevice();

    geode::Result<> start();
    void setAccepting(bool accepting);
    void stop();
    std::vector<float> takeSamples();
    std::uint32_t sampleRate() const;

private:
    MicrophoneCapture() = default;
    MicrophoneCapture(MicrophoneCapture const&) = delete;
    MicrophoneCapture& operator=(MicrophoneCapture const&) = delete;

    geode::Result<> processOnce();
    void workerLoop();
    Device const* activeDevice() const;

    FMOD::System* m_system = nullptr;
    FMOD::Sound* m_sound = nullptr;
    int m_deviceId = -1;
    std::uint32_t m_sampleRate = 48000;
    std::uint32_t m_chunkBytes = 0;
    std::uint32_t m_lastPosition = 0;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_accepting{false};
    std::thread m_worker;
    std::mutex m_mutex;
    std::vector<float> m_samples;
};
