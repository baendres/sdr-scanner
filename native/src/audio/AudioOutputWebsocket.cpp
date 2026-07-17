#include "AudioOutputWebsocket.h"
#include "../dsp/Const.h"

#include <iostream>

namespace sdrscan {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

namespace {
// Matches AudioServer.py's AudioServerOutput_Websocket.SAMPLES_PER_FRAME (~250ms).
constexpr size_t kSamplesPerFrame = AUDIO_SAMPLERATE / 4;
} // namespace

AudioOutputWebsocket::AudioOutputWebsocket(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

AudioOutputWebsocket::~AudioOutputWebsocket() {
    close();
}

void AudioOutputWebsocket::reconnect() {
    close();
    stopFlag_ = false;

    boost::system::error_code ec;
    auto address = host_ == "0.0.0.0" ? boost::asio::ip::address_v4::any()
                                       : boost::asio::ip::make_address(host_, ec);
    tcp::endpoint endpoint(address, static_cast<unsigned short>(port_));

    acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
    acceptor_->open(endpoint.protocol(), ec);
    if (!ec) acceptor_->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (!ec) acceptor_->bind(endpoint, ec);
    if (!ec) acceptor_->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "AudioOutputWebsocket: failed to bind " << host_ << ":" << port_ << ": " << ec.message() << "\n";
        acceptor_.reset();
        return;
    }

    acceptThread_ = std::thread(&AudioOutputWebsocket::acceptLoop, this);
}

void AudioOutputWebsocket::acceptLoop() {
    while (!stopFlag_) {
        boost::system::error_code ec;
        tcp::socket socket(ioc_);
        acceptor_->accept(socket, ec);
        if (ec) {
            if (stopFlag_) break;
            continue;
        }

        auto ws = std::make_shared<WsStream>(std::move(socket));
        try {
            ws->accept();
            ws->binary(true);
        } catch (const std::exception& e) {
            std::cerr << "AudioOutputWebsocket: handshake failed: " << e.what() << "\n";
            continue;
        }

        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.push_back(ws);
    }
}

void AudioOutputWebsocket::close() {
    stopFlag_ = true;
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    acceptor_.reset();

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& c : clients_) {
        boost::system::error_code ec;
        c->close(websocket::close_code::normal, ec);
    }
    clients_.clear();
}

void AudioOutputWebsocket::send(const std::vector<int16_t>& samples) {
    if (samples.empty()) return;

    // Accumulate into ~250ms frames rather than firing a WS message for every tiny batch
    // AudioMixer hands us (it calls send() roughly once per ms) - see the note on
    // outputBuffer_ in the header for why that matters.
    std::vector<std::vector<int16_t>> frames;
    {
        std::lock_guard<std::mutex> lock(outputBufferMutex_);
        outputBuffer_.insert(outputBuffer_.end(), samples.begin(), samples.end());
        while (outputBuffer_.size() >= kSamplesPerFrame) {
            frames.emplace_back(outputBuffer_.begin(), outputBuffer_.begin() + kSamplesPerFrame);
            outputBuffer_.erase(outputBuffer_.begin(), outputBuffer_.begin() + kSamplesPerFrame);
        }
    }
    if (frames.empty()) return;

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const auto& frame : frames) {
        auto it = clients_.begin();
        while (it != clients_.end()) {
            boost::system::error_code ec;
            (*it)->write(boost::asio::buffer(frame.data(), frame.size() * sizeof(int16_t)), ec);
            if (ec) {
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace sdrscan
