#include "AudioOutputWebsocket.h"
#include "../dsp/Const.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

// Upper bound on a client's WS upgrade handshake. A real browser finishes in milliseconds.
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
// Cap on concurrent handshake threads so a connection flood can't exhaust threads/fds.
constexpr int kMaxPendingHandshakes = 32;
// TCP_USER_TIMEOUT for client sockets: if written data sits unacknowledged this long the
// kernel aborts the connection, which makes writerLoop()'s otherwise-unbounded blocking
// write() to a vanished peer fail and return instead of stalling audio for every client.
constexpr unsigned int kTcpUserTimeoutMs = 10000;
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
            // Don't spin on persistent errors (EMFILE etc).
            std::cerr << "AudioOutputWebsocket: accept failed: " << ec.message() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // The accepted socket must be blocking for the sync handshake/writes below.
        socket.non_blocking(false, ec);
        socket.set_option(tcp::no_delay(true), ec);
        socket.set_option(boost::asio::socket_base::keep_alive(true), ec);
        unsigned int userTimeout = kTcpUserTimeoutMs;
        ::setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeout, sizeof(userTimeout));

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pendingHandshakes_ >= kMaxPendingHandshakes) {
                std::cerr << "AudioOutputWebsocket: too many pending handshakes, dropping connection\n";
                continue;  // socket closes on scope exit
            }
            ++pendingHandshakes_;
        }

        // Previously the handshake (ws->accept()) ran right here, synchronously and with no
        // timeout. Any peer that opened a TCP connection and then never sent a complete HTTP
        // upgrade request - an internet port scanner hitting the forwarded port, a phone that
        // lost WiFi mid-connect - blocked this thread forever, so every later audio client sat
        // in the kernel's listen backlog and never got a 101. Control (HttpServer, thread per
        // connection) kept working, which is exactly the observed symptom.
        auto ws = std::make_shared<WsStream>(std::move(socket));
        std::thread(&AudioOutputWebsocket::handshakeClient, this, ws).detach();
    }
}

void AudioOutputWebsocket::handshakeClient(std::shared_ptr<WsStream> ws) {
    auto& socket = beast::get_lowest_layer(*ws);

    // Watchdog: if the handshake hasn't finished in kHandshakeTimeout, shutdown() the socket,
    // which makes the blocking read inside ws->accept() return with an error (same mechanism
    // close() relies on for stuck writes).
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, kHandshakeTimeout, [&] { return done || stopFlag_.load(); }) || stopFlag_) {
            boost::system::error_code ec;
            socket.shutdown(tcp::socket::shutdown_both, ec);
        }
    });

    bool ok = false;
    try {
        ws->accept();
        ws->binary(true);
        ok = true;
    } catch (const std::exception& e) {
        boost::system::error_code ec;
        std::cerr << "AudioOutputWebsocket: handshake failed from "
                  << socket.remote_endpoint(ec) << ": " << e.what() << "\n";
    }

    {
        std::lock_guard<std::mutex> lk(m);
        done = true;
    }
    cv.notify_one();
    watchdog.join();

    if (ok) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        // Re-check under clientsMutex_: close() clears clients_ under the same lock after
        // setting stopFlag_, so a client added after that would never be closed.
        if (!stopFlag_) clients_.push_back(ws);
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        --pendingHandshakes_;
    }
    pendingCv_.notify_all();
}

void AudioOutputWebsocket::close() {
    stopFlag_ = true;
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    if (acceptThread_.joinable()) acceptThread_.join();

    // Wait for in-flight handshake threads (each is bounded by kHandshakeTimeout, and their
    // watchdogs fire immediately once they see stopFlag_ on their next wakeup).
    {
        std::unique_lock<std::mutex> lock(pendingMutex_);
        pendingCv_.wait(lock, [this] { return pendingHandshakes_ == 0; });
    }

    // Must happen before writerThread_.join(), not after: writerLoop() may be blocked in a
    // client's write() against an unresponsive peer right now (dropped WiFi, a suspended
    // browser tab, ...) - see the note on frameQueue_ in the header for the real crash-loop
    // this exact scenario already caused once, on AudioMixer's own thread, before the writer
    // was split onto its own thread. That split doesn't help here: closing these sockets is
    // what makes a stuck write() fail and return, so joining first (the previous order) meant
    // this call - and therefore AudioMixer's shutdown, and therefore the whole process's
    // shutdown/restart - could hang indefinitely the moment a client was in that state.
    //
    // shutdown() before close(): a bare close() from a different thread than the one blocked
    // on the socket isn't a reliably documented way to interrupt it (POSIX leaves this
    // unspecified); shutdown() acts on the connection itself rather than the file descriptor
    // and is the standard, portable way to make a concurrent blocking read/write on the same
    // socket return immediately - see AudioOutputIcecast::close()'s matching fix/note.
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& c : clients_) {
            boost::system::error_code ec;
            auto& socket = beast::get_lowest_layer(*c);
            socket.shutdown(tcp::socket::shutdown_both, ec);
            socket.close(ec);
        }
        clients_.clear();
    }

    if (writerThread_.joinable()) writerThread_.join();
    acceptor_.reset();
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
