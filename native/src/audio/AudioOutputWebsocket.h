#pragma once

#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AudioOutput.h"

namespace sdrscan {

// Streams raw 16-bit signed mono PCM @ AUDIO_SAMPLERATE to any number of connected WebSocket
// clients (e.g. the web UI's browser audio player). Direct port of AudioServer.py's
// AudioServerOutput_Websocket, using Boost.Beast (already a required GNU Radio build
// dependency) instead of Python's `websockets` package.
class AudioOutputWebsocket : public AudioOutput {
public:
    AudioOutputWebsocket(std::string host, int port);
    ~AudioOutputWebsocket() override;

    void reconnect() override;
    void close() override;
    void send(const std::vector<int16_t>& samples) override;

private:
    using WsStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    void acceptLoop();

    std::string host_;
    int port_;

    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::thread acceptThread_;
    std::atomic<bool> stopFlag_{false};

    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<WsStream>> clients_;
};

} // namespace sdrscan
