#pragma once

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../scanner/Scanner.h"
#include "../scanner/ScannerEvent.h"

namespace sdrscan {

// REST + WebSocket control API, direct successor to web_gui.py: serves the (adapted)
// static web UI, GET /api/state for a snapshot, mutating REST endpoints under /api/... for
// every "hot" and "structural" Scanner update, and a /ws WebSocket carrying the same
// broadcast + control-message protocol the Python version used (extended with
// ChannelSetSquelch / ChannelSetCtcss / ChannelSetAudioGain / ChannelSetDwellTime).
//
// Implementation note: uses a synchronous, thread-per-connection Beast server rather than
// Beast's fully-async composed-operation style. Simpler and adequate for a control API with a
// handful of concurrent clients (a couple of browser tabs); revisit if that stops being true.
class HttpServer {
public:
    // requestShutdown, if given, is called when a client hits POST /api/restart (the settings
    // page's "Restart Now" button) - see the comment on that route in HttpServer.cpp for what
    // it's expected to do.
    //
    // authUser/authPassword, if both non-empty, turn on HTTP Basic Auth for every request
    // (static files, REST, and the WS upgrade) - see the comment on requireAuth() in
    // HttpServer.cpp. Off by default (either left empty): this control API has historically had
    // no authentication at all, matching the "trusted home LAN" deployment documented in
    // native/README.md - this only matters once someone exposes the port beyond that LAN (e.g.
    // port-forwarding for remote access, as native/README.md's remote-access section covers).
    HttpServer(Scanner& scanner, std::string host, int port, std::string webRoot,
               std::function<void()> requestShutdown = nullptr,
               std::string authUser = "", std::string authPassword = "");
    ~HttpServer();

    void start();
    void stop();

private:
    using json = nlohmann::json;
    using WsStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    struct WsClient {
        std::shared_ptr<WsStream> ws;
        std::mutex writeMutex;
    };

    // Tracks every accepted connection (HTTP or WS, whichever it ends up being) purely for
    // shutdown safety - closeFn is whatever's currently the live "lowest layer" to close to
    // unstick that connection's thread from a blocking read (the plain tcp::socket before any
    // WS upgrade, or the websocket::stream after one - see handleConnection()). Previously these
    // threads were fire-and-forget (.detach()'d, never tracked at all outside the WS-upgraded
    // case in clients_), which meant a thread still mid-request when the HttpServer is
    // destroyed could go on to touch `this` (scanner_, webRoot_, ...) after destruction.
    struct Connection {
        std::mutex mutex; // guards closeFn against concurrent update-on-upgrade vs. call-from-stop()
        std::function<void()> closeFn;
        std::thread thread;
    };

    void acceptLoop();
    void handleConnection(std::shared_ptr<Connection> conn,
                           std::shared_ptr<boost::asio::ip::tcp::socket> socketPtr);
    // Returns true if the request is authorized (or auth is off) - the caller can proceed.
    // Otherwise writes a 401 response to the socket itself and returns false, so the caller can
    // just `if (!requireAuth(...)) break;`/`return`.
    bool requireAuth(boost::asio::ip::tcp::socket& socket,
                      const boost::beast::http::request<boost::beast::http::string_body>& req);
    void runWsSession(const std::shared_ptr<WsClient>& client);
    void handleWsMessage(const std::shared_ptr<WsClient>& client, const std::string& raw);
    // Enqueues onto writeQueue_ - never writes to the socket itself. See the note on
    // writeQueue_/wsWriterThread_ for why: a caller here can be a receiver's own scan-hopping
    // thread (via the channel-status callback chain), which a blocking write to a stuck client
    // must never be allowed to freeze.
    void sendToClient(const std::shared_ptr<WsClient>& client, const json& msg);
    void wsWriterLoop();

    void onScannerEvent(const ScannerEvent& event);
    void broadcast(const json& msg);
    void applyChannelPatchFields(const std::string& channelId, const json& body);
    void applyReceiverPatchFields(const std::string& receiverId, const json& body);
    void applyOutputPatchFields(int64_t outputId, const json& body);

    Scanner& scanner_;
    std::string host_;
    int port_;
    std::string webRoot_;
    std::function<void()> requestShutdown_;
    // Precomputed "Basic <base64(user:password)>" expected header value; empty means auth is
    // off. See requireAuth() and the constructor comment.
    std::string expectedAuthHeader_;

    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::thread acceptThread_;
    std::atomic<bool> stopFlag_{false};

    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<WsClient>> clients_;

    std::mutex connectionsMutex_;
    std::vector<std::shared_ptr<Connection>> connections_;

    // sendToClient()/broadcast() used to call WsStream::write() directly on whatever thread
    // called them - fine for a request/response handled on its own connection thread, but
    // broadcast() is also reached synchronously from Scanner::emit() via the channel-status
    // callback chain (ChannelBlockBase::reportStatus() -> ... -> SoapyReceiver::
    // checkCurrentWindow()), i.e. on that *receiver's own scan-hopping thread*. write() has no
    // timeout, so one stuck control-WS client (a dropped WiFi connection, a suspended browser
    // tab - the same real scenario already seen on the separate audio WS this session) would
    // silently freeze that receiver's entire loop: no more window hops, no more status updates,
    // no crash and no watchdog to notice (only AudioMixer has a liveness check). This queue +
    // dedicated writer thread decouples every producer from the actual socket write, the same
    // pattern (and for the same reason) as AudioOutputWebsocket's frameQueue_/writerThread_.
    struct PendingWrite {
        std::shared_ptr<WsClient> client;
        std::string payload;
    };
    std::mutex writeQueueMutex_;
    std::deque<PendingWrite> writeQueue_;
    std::thread wsWriterThread_;
};

} // namespace sdrscan
