#include "AudioOutputWebsocket.h"
#include "../dsp/Const.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace sdrscan {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

namespace {
// Batched (rather than firing a WS message per ~1ms AudioMixer tick) to avoid the original
// message-spam bug - see the note on outputBuffer_ in the header. 50ms keeps latency low while
// still batching well below that original per-tick granularity (~20 messages/sec here).
constexpr size_t kSamplesPerFrame = AUDIO_SAMPLERATE / 20;
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
    // Non-blocking so acceptLoop() can poll stopFlag_ instead of sitting in a synchronous
    // accept() that acceptor_->close() from another thread isn't guaranteed to interrupt -
    // same issue as HttpServer::acceptLoop(), see the note there.
    acceptor_->non_blocking(true);

    acceptThread_ = std::thread(&AudioOutputWebsocket::acceptLoop, this);
    writerThread_ = std::thread(&AudioOutputWebsocket::writerLoop, this);
}

void AudioOutputWebsocket::acceptLoop() {
    while (!stopFlag_) {
        boost::system::error_code ec;
        tcp::socket socket(ioc_);
        acceptor_->accept(socket, ec);
        if (ec == boost::asio::error::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
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
    if (writerThread_.joinable()) writerThread_.join();
    acceptor_.reset();

    // Close the raw socket rather than the graceful websocket::stream::close() - the latter
    // waits (no timeout configured) for the peer's close handshake response, which would hang
    // this call indefinitely against a browser tab that's open but not responding. See the
    // matching note in HttpServer::stop() for the (more severe, thread-safety) version of
    // this same issue.
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& c : clients_) {
        boost::system::error_code ec;
        beast::get_lowest_layer(*c).close(ec);
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

    // Just enqueue - writerLoop() on its own thread does the actual (blocking) socket writes.
    // See the header comment on frameQueue_ for why this can't happen here, on AudioMixer's
    // own thread.
    std::lock_guard<std::mutex> lock(frameQueueMutex_);
    for (auto& frame : frames) frameQueue_.push_back(std::move(frame));
}

void AudioOutputWebsocket::writerLoop() {
    while (!stopFlag_) {
        std::vector<int16_t> frame;
        {
            std::lock_guard<std::mutex> lock(frameQueueMutex_);
            if (frameQueue_.empty()) {
                frame.clear();
            } else {
                frame = std::move(frameQueue_.front());
                frameQueue_.erase(frameQueue_.begin());
            }
        }
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Snapshot the client list (cheap - shared_ptr copies) and release clientsMutex_
        // before doing any actual writing. Holding it across a blocking write would mean
        // close() - which also needs this lock to close client sockets and unstick a write
        // that's hung against an unresponsive peer - would itself deadlock waiting on a write
        // it's trying to interrupt.
        std::vector<std::shared_ptr<WsStream>> snapshot;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            snapshot = clients_;
        }

        std::vector<std::shared_ptr<WsStream>> failed;
        for (auto& c : snapshot) {
            boost::system::error_code ec;
            c->write(boost::asio::buffer(frame.data(), frame.size() * sizeof(int16_t)), ec);
            if (ec) failed.push_back(c);
        }

        if (!failed.empty()) {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            for (auto& f : failed) {
                clients_.erase(std::remove(clients_.begin(), clients_.end(), f), clients_.end());
            }
        }
    }
}

} // namespace sdrscan
