#pragma once

#include <deque>
#include <mutex>
#include <string>

#include "AudioOutput.h"

namespace sdrscan {

// Sends raw 16-bit signed mono PCM @ AUDIO_SAMPLERATE to a UDP port. Direct port of
// AudioServer.py's AudioServerOutput_UDP.
class AudioOutputUdp : public AudioOutput {
public:
    AudioOutputUdp(std::string serverIp, int serverPort);
    ~AudioOutputUdp() override;

    void reconnect() override;
    void close() override;
    void send(const std::vector<int16_t>& samples) override;

private:
    std::string serverIp_;
    int serverPort_;
    int socketFd_ = -1;
    std::mutex mutex_;
    std::deque<int16_t> outputBuffer_;
};

} // namespace sdrscan
