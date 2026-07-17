#pragma once

#include <portaudio.h>

#include <chrono>
#include <deque>
#include <mutex>

#include "AudioOutput.h"

namespace sdrscan {

// Plays audio on the host's default output device via PortAudio. Direct port of
// AudioServer.py's AudioServerOutput_Local.
class AudioOutputLocal : public AudioOutput {
public:
    AudioOutputLocal();
    ~AudioOutputLocal() override;

    void reconnect() override;
    void close() override;
    void send(const std::vector<int16_t>& samples) override;

private:
    static int paCallback(const void* input, void* output, unsigned long frameCount,
                           const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags,
                           void* userData);

    std::mutex mutex_;
    std::deque<int16_t> outputBuffer_;
    PaStream* stream_ = nullptr;
    bool paInitialized_ = false;

    // Avoids hammering Pa_OpenDefaultStream (and spamming logs) every ~1ms when there's no
    // usable output device at all - e.g. WSL2 with no host audio, or a genuinely unplugged
    // device. Retries are throttled to once per this interval instead.
    std::chrono::steady_clock::time_point nextReconnectAttempt_{};
    static constexpr std::chrono::seconds kReconnectCooldown{5};
};

} // namespace sdrscan
