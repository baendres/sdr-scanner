#pragma once

#include <boost/asio.hpp>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "AudioOutput.h"

namespace sdrscan {

// Parses "http://host[:port]/mount" - matches AudioServer.py's urlparse-based validation
// (scheme must be http or absent, host required, path must start with "/"). Throws
// std::runtime_error on anything else. Exposed here (rather than file-local) so it has direct
// unit test coverage - it's exactly the kind of small string-parsing logic that's easy to get
// subtly wrong (missing port, missing path, malformed scheme).
struct IcecastUrlParts {
    std::string host;
    int port = 80;
    std::string mount;
};
IcecastUrlParts parseIcecastUrl(const std::string& url);

// Streams MP3-encoded audio to an Icecast-compatible server using the SOURCE protocol
// (Broadcastify expects this - it will not accept HTTP PUT streaming). Direct port of
// AudioServer.py's AudioServerOutput_Icecast: libmp3lame directly instead of Python's lameenc
// wrapper, and a raw Boost.Asio TCP socket for the same manual SOURCE handshake (Beast's HTTP
// client is built around ordinary request/response, not this ICE/1.0 streaming protocol).
class AudioOutputIcecast : public AudioOutput {
public:
    // url must be http://host[:port]/mount (matches the Python version's validation).
    AudioOutputIcecast(std::string url, std::string password);
    ~AudioOutputIcecast() override;

    void reconnect() override;
    void close() override;
    void send(const std::vector<int16_t>& samples) override;

private:
    void runStreamingThread();
    // Returns an open, handshake-accepted socket, or throws std::runtime_error.
    boost::asio::ip::tcp::socket connectSourceSocket();
    void encodeAndSendLoop(boost::asio::ip::tcp::socket& socket);

    std::string host_;
    std::string mount_;
    int port_ = 80;
    std::string authBase64_; // base64("source:" + password), for the SOURCE handshake

    int mp3Bitrate_ = 48000;

    std::mutex bufferMutex_;
    std::deque<int16_t> outputBuffer_;
    size_t bufferMaxLen_;

    boost::asio::io_context ioc_;
    std::atomic<bool> stopFlag_{true};
    std::thread streamingThread_;
};

} // namespace sdrscan
