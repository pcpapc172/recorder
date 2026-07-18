#include "MicrophoneCapture.hpp"

#include <Geode/binding/FMODAudioEngine.hpp>
#include <fmod_errors.h>
#ifdef GEODE_IS_ANDROID
#include <Geode/utils/permission.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

using namespace geode::prelude;

MicrophoneCapture& MicrophoneCapture::get() {
    static MicrophoneCapture instance;
    return instance;
}

std::vector<MicrophoneCapture::Device> MicrophoneCapture::enumerateDevices() {
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return {};

    int count = 0;
    int connected = 0;
    if (engine->m_system->getRecordNumDrivers(&count, &connected) != FMOD_OK) return {};

    std::vector<Device> devices;
    for (int id = 0; id < count; ++id) {
        Device device;
        char name[256]{};
        FMOD_SPEAKERMODE mode{};
        int channels = 0;
        FMOD_DRIVER_STATE state{};
        if (engine->m_system->getRecordDriverInfo(
            id, name, sizeof(name), &device.guid, &device.sampleRate,
            &mode, &channels, &state
        ) == FMOD_OK) {
            device.id = id;
            device.name = name;
            devices.push_back(std::move(device));
        }
    }
    return devices;
}

std::string MicrophoneCapture::guidString(FMOD_GUID const& guid) {
    return fmt::format(
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]
    );
}

MicrophoneCapture::Device const* MicrophoneCapture::activeDevice() const {
    static thread_local Device selected;
    auto devices = enumerateDevices();
    if (devices.empty()) return nullptr;

    auto configured = Mod::get()->getSettingValue<std::string>("microphone-device-guid");
    auto it = std::find_if(devices.begin(), devices.end(), [&](Device const& device) {
        return !configured.empty() && guidString(device.guid) == configured;
    });
    selected = it == devices.end() ? devices.front() : *it;
    return &selected;
}

std::string MicrophoneCapture::selectedDeviceName() {
    auto devices = enumerateDevices();
    if (devices.empty()) return "No microphone";
    auto configured = Mod::get()->getSettingValue<std::string>("microphone-device-guid");
    auto it = std::find_if(devices.begin(), devices.end(), [&](Device const& device) {
        return !configured.empty() && guidString(device.guid) == configured;
    });
    return (it == devices.end() ? devices.front() : *it).name;
}

void MicrophoneCapture::cycleDevice() {
    auto devices = enumerateDevices();
    if (devices.empty()) return;
    auto configured = Mod::get()->getSettingValue<std::string>("microphone-device-guid");
    auto it = std::find_if(devices.begin(), devices.end(), [&](Device const& device) {
        return !configured.empty() && guidString(device.guid) == configured;
    });
    auto index = it == devices.end() ? 0u : static_cast<std::size_t>(it - devices.begin());
    index = (index + 1) % devices.size();
    Mod::get()->setSettingValue<std::string>("microphone-device-guid", guidString(devices[index].guid));
}

Result<> MicrophoneCapture::start() {
    stop();
#ifdef GEODE_IS_ANDROID
    namespace permission = geode::utils::permission;
    using permission::Permission;
    if (!permission::getPermissionStatus(Permission::RecordAudio)) {
        permission::requestPermission(Permission::RecordAudio, [](bool granted) {
            if (granted) {
                log::info("Android microphone permission granted");
            } else {
                log::warn("Android microphone permission denied");
            }
        });
        return Err("Microphone permission was requested. Allow it, then start recording again.");
    }
#endif
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return Err("FMOD audio engine is not available.");
    auto device = activeDevice();
    if (!device) return Err("No microphone device was found.");

    m_system = engine->m_system;
    m_deviceId = device->id;
    m_sampleRate = static_cast<std::uint32_t>(std::max(device->sampleRate, 8000));
    m_chunkBytes = sizeof(float) * m_sampleRate;
    FMOD_CREATESOUNDEXINFO info{};
    info.cbsize = sizeof(info);
    info.numchannels = 1;
    info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    info.defaultfrequency = static_cast<int>(m_sampleRate);
    info.length = m_chunkBytes;

    auto result = m_system->createSound(
        nullptr, FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL | FMOD_CREATESAMPLE,
        &info, &m_sound
    );
    if (result != FMOD_OK || !m_sound) return Err("FMOD could not create the microphone buffer.");
    if ((result = m_system->recordStart(m_deviceId, m_sound, true)) != FMOD_OK) {
        m_sound->release();
        m_sound = nullptr;
        return Err("FMOD could not start the selected microphone: {}", FMOD_ErrorString(result));
    }

    {
        std::scoped_lock lock(m_mutex);
        m_samples.clear();
        if (m_samples.capacity() < 8u * 1024u * 1024u) m_samples.reserve(8u * 1024u * 1024u);
    }
    m_lastPosition = 0;
    m_accepting.store(true, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    try {
        m_worker = std::thread(&MicrophoneCapture::workerLoop, this);
    } catch (std::system_error const& error) {
        stop();
        return Err("Unable to create microphone worker: {}", error.what());
    }
    log::info("Microphone recording started: {}", device->name);
    return Ok();
}

void MicrophoneCapture::setAccepting(bool accepting) {
    m_accepting.store(accepting, std::memory_order_release);
}

Result<> MicrophoneCapture::processOnce() {
    if (!m_system || !m_sound || m_deviceId < 0) return Ok();
    unsigned int position = 0;
    auto result = m_system->getRecordPosition(m_deviceId, &position);
    if (result == FMOD_ERR_RECORD_DISCONNECTED || result != FMOD_OK || position == m_lastPosition) return Ok();

    float* pcm = nullptr;
    unsigned int length = 0;
    result = m_sound->lock(0, m_chunkBytes, reinterpret_cast<void**>(&pcm), nullptr, &length, nullptr);
    if (result != FMOD_OK || !pcm) return Ok();
    auto totalSamples = length / sizeof(float);
    auto append = [&](float const* data, std::size_t count) {
        if (!m_accepting.load(std::memory_order_acquire)) return;
        std::scoped_lock lock(m_mutex);
        if (m_samples.size() + count <= m_samples.capacity()) m_samples.insert(m_samples.end(), data, data + count);
    };
    if (position > m_lastPosition) {
        append(pcm + m_lastPosition, position - m_lastPosition);
    } else {
        if (m_lastPosition < totalSamples) append(pcm + m_lastPosition, totalSamples - m_lastPosition);
        append(pcm, position);
    }
    m_lastPosition = position;
    (void)m_sound->unlock(pcm, nullptr, length, 0);
    return Ok();
}

void MicrophoneCapture::workerLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        (void)processOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

void MicrophoneCapture::stop() {
    if (!m_system || m_deviceId < 0) return;
    m_running.store(false, std::memory_order_release);
    if (m_worker.joinable()) m_worker.join();
    (void)m_system->recordStop(m_deviceId);
    (void)processOnce();
    m_accepting.store(false, std::memory_order_release);
    if (m_sound) m_sound->release();
    m_sound = nullptr;
    m_system = nullptr;
    m_deviceId = -1;
}

std::vector<float> MicrophoneCapture::takeSamples() {
    std::scoped_lock lock(m_mutex);
    return std::exchange(m_samples, std::vector<float>());
}

std::uint32_t MicrophoneCapture::sampleRate() const {
    return m_sampleRate;
}
