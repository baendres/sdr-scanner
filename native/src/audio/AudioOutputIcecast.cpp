#include "AudioOutputIcecast.h"
#include "../dsp/Const.h"
#include "../util/Base64.h"

#include <lame/lame.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace sdrscan {

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr int kSamplesPerFrame = AUDIO_SAMPLERATE / 4;
} // namespace

IcecastUrlParts parseIcecastUrl(const std::string& url) {
    std::string rest = url;
    if (rest.compare(0, 7, "http://") == 0) {
        rest = rest.substr(7);
    } else if (rest.find("://") != std::string::npos) {
        throw std::runtime_error("Icecast URL must be http://host:port/mount (got: " + url + ")");
    }

    auto slashPos = rest.find('/');
    std::string hostPort = (slashPos == std::string::npos) ? rest : rest.substr(0, slashPos);
    std::string mount = (slashPos == std::string::npos) ? "/" : rest.substr(slashPos);
    if (mount.empty() || mount[0] != '/') {
        throw std::runtime_error("Invalid Icecast URL: " + url);
    }

    IcecastUrlParts parsed;
    auto colonPos = hostPort.find(':');
    if (colonPos == std::string::npos) {
        parsed.host = hostPort;
    } else {
        parsed.host = hostPort.substr(0, colonPos);
        parsed.port = std::stoi(hostPort.substr(colonPos + 1));
    }
    parsed.mount = mount;
    if (parsed.host.empty()) {
        throw std::runtime_error("Invalid Icecast URL: " + url);
    }
    return parsed;
}

AudioOutputIcecast::AudioOutputIcecast(std::string url, std::string password)
    : bufferMaxLen_(static_cast<size_t>(kSamplesPerFrame) * 3) {
    IcecastUrlParts parsed = parseIcecastUrl(url);
    host_ = parsed.host;
    port_ = parsed.port;
    mount_ = parsed.mount;
    authBase64_ = base64Encode("source:" + password);
}

AudioOutputIcecast::~AudioOutputIcecast() {
    close();
}

void AudioOutputIcecast::reconnect() {
    close();
    stopFlag_ = false;
    streamingThread_ = std::thread(&AudioOutputIcecast::runStreamingThread, this);
}

void AudioOutputIcecast::close() {
    stopFlag_ = true;
    if (streamingThread_.joinable()) streamingThread_.join();
}

void AudioOutputIcecast::send(const std::vector<int16_t>& samples) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    for (int16_t s : samples) {
        if (outputBuffer_.size() >= bufferMaxLen_) outputBuffer_.pop_front();
        outputBuffer_.push_back(s);
    }
}

boost::asio::ip::tcp::socket AudioOutputIcecast::connectSourceSocket() {
    std::cout << "AudioOutputIcecast: connecting to " << host_ << ":" << port_ << mount_ << "\n";

    tcp::socket socket(ioc_);
    tcp::resolver resolver(ioc_);
    asio::connect(socket, resolver.resolve(host_, std::to_string(port_)));
    socket.set_option(tcp::no_delay(true));

    // Minimal SOURCE handshake, matching the Python version - "ICE/1.0" (not "HTTP/1.0") is
    // important for many Icecast implementations to recognize this as a SOURCE request.
    std::string req = "SOURCE " + mount_ + " ICE/1.0\r\n"
                       "Host: " + host_ + "\r\n"
                       "Authorization: Basic " + authBase64_ + "\r\n"
                       "Content-Type: audio/mpeg\r\n"
                       "User-Agent: sdr-scanner\r\n"
                       "Ice-Name: sdr-scanner\r\n"
                       "\r\n";
    asio::write(socket, asio::buffer(req));

    // Read the response until the blank-line header terminator.
    asio::streambuf buf;
    boost::system::error_code ec;
    asio::read_until(socket, buf, "\r\n\r\n", ec);
    if (ec) {
        asio::read_until(socket, buf, "\n\n", ec); // rare alternate terminator
        if (ec) throw std::runtime_error("Icecast: failed reading response headers: " + ec.message());
    }

    std::istream is(&buf);
    std::string statusLine;
    std::getline(is, statusLine);
    // Typical: "HTTP/1.0 200 OK" or "ICY 200 OK" - status code is always the second token.
    int code = 0;
    auto firstSpace = statusLine.find(' ');
    if (firstSpace != std::string::npos) {
        try {
            code = std::stoi(statusLine.substr(firstSpace + 1));
        } catch (const std::exception&) {
        }
    }
    if (code != 200) {
        boost::system::error_code ignore;
        socket.close(ignore);
        throw std::runtime_error("Icecast SOURCE rejected: " + statusLine);
    }

    std::cout << "AudioOutputIcecast: SOURCE connected: " << statusLine << "\n";
    return socket;
}

void AudioOutputIcecast::encodeAndSendLoop(boost::asio::ip::tcp::socket& socket) {
    lame_t lame = lame_init();
    if (!lame) throw std::runtime_error("Icecast: failed to initialize MP3 encoder");

    lame_set_num_channels(lame, 1);
    lame_set_in_samplerate(lame, AUDIO_SAMPLERATE);
    lame_set_brate(lame, mp3Bitrate_ / 1000);
    lame_set_mode(lame, MONO);
    lame_set_quality(lame, 2);
    if (lame_init_params(lame) < 0) {
        lame_close(lame);
        throw std::runtime_error("Icecast: MP3 encoder parameter init failed");
    }

    std::vector<int16_t> pcmFrame(kSamplesPerFrame);
    // MP3 output can briefly exceed input size (encoder buffering/flushing) - matches LAME's
    // own documented worst-case sizing guidance (1.25x input samples, plus a fixed overhead).
    std::vector<unsigned char> mp3Buf(static_cast<size_t>(kSamplesPerFrame) * 5 / 4 + 7200);

    while (!stopFlag_) {
        bool haveFrame = false;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            if (outputBuffer_.size() >= static_cast<size_t>(kSamplesPerFrame)) {
                for (int i = 0; i < kSamplesPerFrame; i++) {
                    pcmFrame[i] = outputBuffer_.front();
                    outputBuffer_.pop_front();
                }
                haveFrame = true;
            }
        }
        if (!haveFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int n = lame_encode_buffer(lame, pcmFrame.data(), nullptr, kSamplesPerFrame, mp3Buf.data(), static_cast<int>(mp3Buf.size()));
        if (n < 0) throw std::runtime_error("Icecast: MP3 encoding error " + std::to_string(n));
        if (n > 0) {
            boost::system::error_code ec;
            asio::write(socket, asio::buffer(mp3Buf.data(), static_cast<size_t>(n)), ec);
            if (ec) {
                lame_close(lame);
                throw std::runtime_error("Icecast: send failed: " + ec.message());
            }
        }
    }

    lame_close(lame);
}

void AudioOutputIcecast::runStreamingThread() {
    constexpr auto kReconnectBackoff = std::chrono::seconds(5);

    while (!stopFlag_) {
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            outputBuffer_.clear();
        }

        try {
            tcp::socket socket = connectSourceSocket();
            encodeAndSendLoop(socket);
            boost::system::error_code ec;
            socket.shutdown(tcp::socket::shutdown_both, ec);
        } catch (const std::exception& e) {
            if (!stopFlag_) std::cerr << "AudioOutputIcecast: " << e.what() << "\n";
        }

        auto deadline = std::chrono::steady_clock::now() + kReconnectBackoff;
        while (!stopFlag_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

} // namespace sdrscan
