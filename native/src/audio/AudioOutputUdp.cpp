#include "AudioOutputUdp.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace sdrscan {

namespace {
constexpr size_t kSamplesPerPacket = 100;
}

AudioOutputUdp::AudioOutputUdp(std::string serverIp, int serverPort)
    : serverIp_(std::move(serverIp)), serverPort_(serverPort) {}

AudioOutputUdp::~AudioOutputUdp() {
    close();
}

void AudioOutputUdp::reconnect() {
    close();
    socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd_ < 0) {
        std::cerr << "AudioOutputUdp: failed to create socket\n";
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    outputBuffer_.clear();
}

void AudioOutputUdp::close() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
}

void AudioOutputUdp::send(const std::vector<int16_t>& samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    outputBuffer_.insert(outputBuffer_.end(), samples.begin(), samples.end());

    if (socketFd_ < 0) {
        reconnect();
        if (socketFd_ < 0) return;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(serverPort_));
    if (inet_pton(AF_INET, serverIp_.c_str(), &dest.sin_addr) != 1) {
        std::cerr << "AudioOutputUdp: invalid server IP: " << serverIp_ << "\n";
        return;
    }

    while (outputBuffer_.size() > kSamplesPerPacket) {
        int16_t packet[kSamplesPerPacket];
        for (size_t i = 0; i < kSamplesPerPacket; i++) {
            packet[i] = outputBuffer_.front();
            outputBuffer_.pop_front();
        }
        ssize_t sent = sendto(socketFd_, packet, sizeof(packet), 0,
                               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent < 0) {
            std::cerr << "AudioOutputUdp: send failed, reconnecting\n";
            reconnect();
            return;
        }
    }
}

} // namespace sdrscan
