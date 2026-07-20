#pragma once

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <atomic>
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
    HttpServer(Scanner& scanner, std::string host, int port, std::string webRoot);
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

    void acceptLoop();
    void handleConnection(boost::asio::ip::tcp::socket socket);
    void runWsSession(const std::shared_ptr<WsClient>& client);
    void handleWsMessage(const std::shared_ptr<WsClient>& client, const std::string& raw);
    void sendToClient(const std::shared_ptr<WsClient>& client, const json& msg);

    void onScannerEvent(const ScannerEvent& event);
    void broadcast(const json& msg);
    void applyChannelPatchFields(const std::string& channelId, const json& body);
    void applyReceiverPatchFields(const std::string& receiverId, const json& body);
    void applyOutputPatchFields(int64_t outputId, const json& body);

    Scanner& scanner_;
    std::string host_;
    int port_;
    std::string webRoot_;

    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::thread acceptThread_;
    std::atomic<bool> stopFlag_{false};

    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<WsClient>> clients_;
};

} // namespace sdrscan
