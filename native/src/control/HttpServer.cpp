#include "HttpServer.h"
#include "Protocol.h"
#include "../receiver/SoapyReceiver.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace sdrscan {

namespace {
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

std::string contentTypeFor(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
    return "application/octet-stream";
}

http::response<http::string_body> jsonResponse(unsigned version, http::status status, const nlohmann::json& body) {
    http::response<http::string_body> res{status, version};
    res.set(http::field::content_type, "application/json");
    res.body() = body.dump();
    res.prepare_payload();
    return res;
}

http::response<http::string_body> errorResponse(unsigned version, http::status status, const std::string& message) {
    return jsonResponse(version, status, nlohmann::json{{"error", message}});
}

// Not actually channel-specific despite the name - just strips a path prefix. Kept the name
// since it's the one already used throughout for /api/channels/{id}.
std::string channelIdFromPath(const std::string& target, const std::string& prefix) {
    if (target.size() <= prefix.size() || target.compare(0, prefix.size(), prefix) != 0) return "";
    return target.substr(prefix.size());
}

} // namespace

HttpServer::HttpServer(Scanner& scanner, std::string host, int port, std::string webRoot,
                       std::function<void()> requestShutdown)
    : scanner_(scanner), host_(std::move(host)), port_(port), webRoot_(std::move(webRoot)),
      requestShutdown_(std::move(requestShutdown)) {
    scanner_.setEventCallback([this](const ScannerEvent& event) { onScannerEvent(event); });
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    boost::system::error_code ec;
    auto address = host_ == "0.0.0.0" ? boost::asio::ip::address_v4::any() : boost::asio::ip::make_address(host_, ec);
    tcp::endpoint endpoint(address, static_cast<unsigned short>(port_));

    acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
    acceptor_->open(endpoint.protocol(), ec);
    if (!ec) acceptor_->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (!ec) acceptor_->bind(endpoint, ec);
    if (!ec) acceptor_->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("HttpServer: failed to bind " + host_ + ":" + std::to_string(port_) + ": " + ec.message());
    }
    // Non-blocking so acceptLoop() can poll stopFlag_ instead of sitting in a synchronous
    // accept() that acceptor_->close() from another thread isn't guaranteed to interrupt
    // (Asio only documents cancellation for asynchronous operations) - without this, stop()
    // could hang forever in acceptThread_.join().
    acceptor_->non_blocking(true);

    stopFlag_ = false;
    acceptThread_ = std::thread(&HttpServer::acceptLoop, this);
    std::cout << "HttpServer listening on " << host_ << ":" << port_ << "\n";
}

void HttpServer::stop() {
    stopFlag_ = true;
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    acceptor_.reset();

    // NOTE: deliberately closing the raw socket (lowest layer) rather than doing a graceful
    // websocket::stream::close() here. Each client's session thread (runWsSession, still
    // running - these are detached, not tracked/joined) is normally blocked in
    // client->ws->read(...) on this exact same stream object. Beast's websocket::stream is
    // not safe for concurrent operations from two threads, and close() also waits
    // (unbounded, no timeout configured) for the peer's close handshake response - either of
    // those was enough to hang this call indefinitely whenever a browser tab was still
    // connected, which is why Ctrl+C never actually exited the process. Closing the
    // underlying socket instead just makes the session thread's pending read() fail
    // immediately, which is thread-safe and unblocks it without a handshake round-trip.
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& c : clients_) {
        boost::system::error_code ec;
        beast::get_lowest_layer(*c->ws).close(ec);
    }
    clients_.clear();
}

void HttpServer::acceptLoop() {
    while (!stopFlag_) {
        boost::system::error_code ec;
        tcp::socket socket(ioc_);
        acceptor_->accept(socket, ec);
        if (ec == boost::asio::error::would_block) {
            // No connection pending right now (see the non_blocking(true) note in start()) -
            // brief sleep so this polls stopFlag_ regularly without busy-spinning the CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ec) {
            if (stopFlag_) break;
            continue;
        }
        std::thread(&HttpServer::handleConnection, this, std::move(socket)).detach();
    }
}

void HttpServer::handleConnection(tcp::socket socket) {
    try {
        beast::flat_buffer buffer;
        for (;;) {
            http::request<http::string_body> req;
            http::read(socket, buffer, req);

            if (websocket::is_upgrade(req)) {
                auto client = std::make_shared<WsClient>();
                client->ws = std::make_shared<WsStream>(std::move(socket));
                try {
                    client->ws->accept(req);
                } catch (const std::exception& e) {
                    std::cerr << "HttpServer: WS handshake failed: " << e.what() << "\n";
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(clientsMutex_);
                    clients_.push_back(client);
                }
                runWsSession(client);
                return; // socket ownership moved into the WS stream
            }

            std::string target(req.target());
            std::string path = target;
            auto qpos = path.find('?');
            if (qpos != std::string::npos) path = path.substr(0, qpos);

            http::response<http::string_body> res;
            nlohmann::json body;
            bool haveBody = false;
            if (!req.body().empty()) {
                try {
                    body = nlohmann::json::parse(req.body());
                    haveBody = true;
                } catch (const nlohmann::json::exception&) {
                    res = errorResponse(req.version(), http::status::bad_request, "invalid JSON body");
                    haveBody = false;
                }
            }

            if (!(res.result() == http::status::bad_request)) {
                try {
                    if (req.method() == http::verb::get && path == "/api/state") {
                        res = jsonResponse(req.version(), http::status::ok, protocol::snapshotToJson(scanner_.getSnapshot()));
                    } else if (req.method() == http::verb::patch && path.rfind("/api/channels/", 0) == 0) {
                        std::string id = channelIdFromPath(path, "/api/channels/");
                        applyChannelPatchFields(id, haveBody ? body : nlohmann::json::object());
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::post && path == "/api/channels") {
                        auto cc = protocol::channelConfigFromJson(haveBody ? body : nlohmann::json::object());
                        std::string id = scanner_.addChannel(cc);
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"id", id}});
                    } else if (req.method() == http::verb::delete_ && path.rfind("/api/channels/", 0) == 0) {
                        std::string id = channelIdFromPath(path, "/api/channels/");
                        scanner_.removeChannel(id);
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::patch && path == "/api/scanner") {
                        if (haveBody && body.contains("maxChannelsPerWindow")) {
                            scanner_.setMaxChannelsPerWindow(body.at("maxChannelsPerWindow").get<int>());
                        }
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::get && path == "/api/receivers/scan") {
                        nlohmann::json devices = nlohmann::json::array();
                        for (const auto& kwargs : SoapyReceiver::scanAvailableDevices()) {
                            devices.push_back(protocol::soapyDeviceToJson(kwargs));
                        }
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"devices", devices}});
                    } else if (req.method() == http::verb::post && path == "/api/receivers") {
                        auto rc = protocol::receiverConfigFromJson(haveBody ? body : nlohmann::json::object());
                        std::string id = scanner_.upsertReceiverConfig(rc);
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"id", id}});
                    } else if (req.method() == http::verb::patch && path.rfind("/api/receivers/", 0) == 0) {
                        std::string id = channelIdFromPath(path, "/api/receivers/");
                        applyReceiverPatchFields(id, haveBody ? body : nlohmann::json::object());
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::delete_ && path.rfind("/api/receivers/", 0) == 0) {
                        std::string id = channelIdFromPath(path, "/api/receivers/");
                        scanner_.deleteReceiverConfig(id);
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::post && path == "/api/outputs") {
                        auto oc = protocol::outputConfigFromJson(haveBody ? body : nlohmann::json::object());
                        int64_t id = scanner_.upsertOutputConfig(oc);
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"id", id}});
                    } else if (req.method() == http::verb::patch && path.rfind("/api/outputs/", 0) == 0) {
                        int64_t id = std::stoll(channelIdFromPath(path, "/api/outputs/"));
                        applyOutputPatchFields(id, haveBody ? body : nlohmann::json::object());
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::delete_ && path.rfind("/api/outputs/", 0) == 0) {
                        int64_t id = std::stoll(channelIdFromPath(path, "/api/outputs/"));
                        scanner_.deleteOutputConfig(id);
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                    } else if (req.method() == http::verb::post && path == "/api/restart") {
                        // The settings page's "Restart Now" button, for applying database-only
                        // receiver/output changes (see applyReceiverPatchFields's comment). This
                        // process just exits gracefully - main.cpp's requestShutdown callback
                        // sets the same flag Ctrl+C/SIGTERM does - and relies on the deployment's
                        // restart policy (docker-compose's `restart: unless-stopped`) to bring it
                        // back up with the new config loaded. Written and sent before the
                        // shutdown is requested, on this same (per-connection) thread, so the
                        // client always gets a response even though the server process is about
                        // to go away.
                        res = jsonResponse(req.version(), http::status::ok, nlohmann::json{{"ok", true}});
                        if (requestShutdown_) requestShutdown_();
                    } else if (req.method() == http::verb::get) {
                        std::string filePath = path == "/" ? "index.html" : path.substr(1);
                        if (filePath.find("..") != std::string::npos) {
                            res = errorResponse(req.version(), http::status::bad_request, "invalid path");
                        } else {
                            std::ifstream file(webRoot_ + "/" + filePath, std::ios::binary);
                            if (!file) {
                                res = errorResponse(req.version(), http::status::not_found, "not found");
                            } else {
                                std::ostringstream ss;
                                ss << file.rdbuf();
                                res = http::response<http::string_body>{http::status::ok, req.version()};
                                res.set(http::field::content_type, contentTypeFor(filePath));
                                // Never let the browser cache the web UI's own files. This app
                                // gets rebuilt/redeployed in place (docker compose up --build)
                                // while a tab may already be open - without this, a browser can
                                // keep running old JS/CSS indefinitely (nothing here ever tells
                                // it to check again), which looks exactly like a fix "not
                                // working" when it's really just not loaded yet.
                                res.set(http::field::cache_control, "no-store");
                                res.body() = ss.str();
                                res.prepare_payload();
                            }
                        }
                    } else {
                        res = errorResponse(req.version(), http::status::not_found, "not found");
                    }
                } catch (const std::exception& e) {
                    res = errorResponse(req.version(), http::status::bad_request, e.what());
                }
            }

            res.keep_alive(req.keep_alive());
            http::write(socket, res);
            if (!req.keep_alive()) break;
        }
    } catch (const std::exception&) {
        // connection closed / read error - normal at EOF, nothing to do
    }
}

void HttpServer::applyChannelPatchFields(const std::string& channelId, const json& body) {
    // Structural fields need a full-record edit + window rebuild rather than a per-field "hot"
    // setter. Applied first from the pre-patch snapshot so any hot fields also present in the
    // same PATCH body still win below (they're applied after, straight to the live block).
    if (body.contains("freq_hz") || body.contains("label") || body.contains("mode")) {
        auto snapshot = scanner_.getSnapshot();
        auto it = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
                                [&](const ChannelConfig& cc) { return cc.id == channelId; });
        if (it == snapshot.channels.end()) throw std::runtime_error("Channel not found: " + channelId);
        ChannelConfig cc = *it;
        if (body.contains("freq_hz")) cc.freq_hz = body.at("freq_hz").get<int64_t>();
        if (body.contains("label")) cc.label = body.at("label").get<std::string>();
        if (body.contains("mode")) {
            auto mode = channelModeFromString(body.at("mode").get<std::string>());
            if (!mode) throw std::runtime_error("Unknown channel mode");
            cc.mode = *mode;
        }
        scanner_.editChannel(cc);
    }

    if (body.contains("squelchThreshold")) scanner_.setChannelSquelch(channelId, body.at("squelchThreshold").get<double>());
    if (body.contains("ctcssToneHz")) {
        auto v = body.at("ctcssToneHz");
        scanner_.setChannelCtcssTone(channelId, v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
    }
    if (body.contains("squelchNoiseMargin_dB")) {
        auto v = body.at("squelchNoiseMargin_dB");
        scanner_.setChannelSquelchNoiseMargin(channelId, v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
    }
    if (body.contains("noiseSquelchThreshold_dB")) {
        auto v = body.at("noiseSquelchThreshold_dB");
        scanner_.setChannelNoiseSquelchThreshold(channelId, v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
    }
    if (body.contains("audioGain_dB")) scanner_.setChannelAudioGain(channelId, body.at("audioGain_dB").get<double>());
    if (body.contains("dwellTime_s")) scanner_.setChannelDwellTime(channelId, body.at("dwellTime_s").get<double>());
    if (body.contains("mute")) scanner_.setChannelMute(channelId, body.at("mute").get<bool>());
    if (body.contains("solo")) scanner_.setChannelSolo(channelId, protocol::jsonToTriBool(body.at("solo")));
    if (body.contains("hold")) scanner_.setChannelHold(channelId, body.at("hold").get<bool>());
    if (body.contains("forceActive")) scanner_.setChannelForceActive(channelId, body.at("forceActive").get<bool>());
    if (body.contains("enabled")) scanner_.setChannelEnabled(channelId, body.at("enabled").get<bool>());
    if (body.contains("disableUntil") && !body.at("disableUntil").is_null()) {
        scanner_.setChannelDisableUntil(channelId, body.at("disableUntil").get<double>());
    }
}

// Receiver/output PATCH: restart-to-apply (see Scanner::upsertReceiverConfig's header note), so
// this just merges the patch fields into the existing database record and re-upserts - no live
// hardware/output reconfiguration involved.
void HttpServer::applyReceiverPatchFields(const std::string& receiverId, const json& body) {
    auto snapshot = scanner_.getSnapshot();
    auto it = std::find_if(snapshot.receivers.begin(), snapshot.receivers.end(),
                            [&](const ReceiverConfig& rc) { return rc.id == receiverId; });
    if (it == snapshot.receivers.end()) throw std::runtime_error("Receiver not found: " + receiverId);
    json merged = protocol::receiverConfigToJson(*it);
    for (auto& [k, v] : body.items()) merged[k] = v;
    merged["id"] = receiverId;
    scanner_.upsertReceiverConfig(protocol::receiverConfigFromJson(merged));
}

void HttpServer::applyOutputPatchFields(int64_t outputId, const json& body) {
    auto snapshot = scanner_.getSnapshot();
    auto it = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                            [&](const OutputConfig& oc) { return oc.id == outputId; });
    if (it == snapshot.outputs.end()) throw std::runtime_error("Output not found: " + std::to_string(outputId));
    json merged = protocol::outputConfigToJson(*it);
    for (auto& [k, v] : body.items()) merged[k] = v;
    merged["id"] = outputId;
    scanner_.upsertOutputConfig(protocol::outputConfigFromJson(merged));
}

void HttpServer::sendToClient(const std::shared_ptr<WsClient>& client, const json& msg) {
    std::lock_guard<std::mutex> lock(client->writeMutex);
    boost::system::error_code ec;
    std::string payload = msg.dump();
    client->ws->text(true);
    client->ws->write(boost::asio::buffer(payload), ec);
}

void HttpServer::runWsSession(const std::shared_ptr<WsClient>& client) {
    sendToClient(client, json{{"type", "Snapshot"}, {"data", protocol::snapshotToJson(scanner_.getSnapshot())}});

    beast::flat_buffer buffer;
    while (!stopFlag_) {
        boost::system::error_code ec;
        client->ws->read(buffer, ec);
        if (ec) break;
        std::string raw = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        handleWsMessage(client, raw);
    }

    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

void HttpServer::handleWsMessage(const std::shared_ptr<WsClient>& client, const std::string& raw) {
    json msg;
    try {
        msg = json::parse(raw);
    } catch (const json::exception&) {
        sendToClient(client, json{{"type", "Error"}, {"data", {{"error", "invalid json"}}}});
        return;
    }
    if (!msg.is_object() || !msg.contains("type")) {
        sendToClient(client, json{{"type", "Error"}, {"data", {{"error", "invalid message"}}}});
        return;
    }

    std::string type = msg.value("type", "");
    json data = msg.value("data", json::object());

    try {
        if (type == "Ping") {
            // No-op besides the Ack below - a periodic client-side keepalive (see app.js's
            // connectWS()) so an idle control connection (quiet whenever nothing's actually
            // changing) doesn't get silently dropped by an intermediate reverse proxy's idle
            // connection timeout, which was observed on real hardware as the panel repeatedly
            // showing "disconnected - retrying" despite the server itself never having crashed
            // or restarted.
        } else if (type == "ChannelMute") {
            scanner_.setChannelMute(data.at("id").get<std::string>(), data.at("mute").get<bool>());
        } else if (type == "ChannelSolo") {
            scanner_.setChannelSolo(data.at("id").get<std::string>(), protocol::jsonToTriBool(data.at("solo")));
        } else if (type == "ChannelHold") {
            scanner_.setChannelHold(data.at("id").get<std::string>(), data.at("hold").get<bool>());
        } else if (type == "ChannelEnable") {
            scanner_.setChannelEnabled(data.at("id").get<std::string>(), data.at("enabled").get<bool>());
        } else if (type == "ChannelDisableUntil") {
            scanner_.setChannelDisableUntil(data.at("id").get<std::string>(), data.at("disableUntil").get<double>());
        } else if (type == "ChannelForceActive") {
            scanner_.setChannelForceActive(data.at("id").get<std::string>(), data.at("forceActive").get<bool>());
        } else if (type == "ChannelSetSquelch") {
            scanner_.setChannelSquelch(data.at("id").get<std::string>(), data.at("squelchThreshold").get<double>());
        } else if (type == "ChannelSetCtcss") {
            auto v = data.at("ctcssToneHz");
            scanner_.setChannelCtcssTone(data.at("id").get<std::string>(), v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
        } else if (type == "ChannelSetSquelchNoiseMargin") {
            auto v = data.at("squelchNoiseMargin_dB");
            scanner_.setChannelSquelchNoiseMargin(data.at("id").get<std::string>(), v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
        } else if (type == "ChannelSetNoiseSquelchThreshold") {
            auto v = data.at("noiseSquelchThreshold_dB");
            scanner_.setChannelNoiseSquelchThreshold(data.at("id").get<std::string>(), v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
        } else if (type == "ChannelSetAudioGain") {
            scanner_.setChannelAudioGain(data.at("id").get<std::string>(), data.at("audioGain_dB").get<double>());
        } else if (type == "ChannelSetDwellTime") {
            scanner_.setChannelDwellTime(data.at("id").get<std::string>(), data.at("dwellTime_s").get<double>());
        } else {
            sendToClient(client, json{{"type", "Error"}, {"data", {{"error", "unknown type"}, {"messageType", type}}}});
            return;
        }
        sendToClient(client, json{{"type", "Ack"}, {"data", {{"ok", true}, {"echoType", type}}}});
    } catch (const std::exception& e) {
        sendToClient(client, json{{"type", "Error"}, {"data", {{"error", e.what()}, {"messageType", type}}}});
    }
}

void HttpServer::onScannerEvent(const ScannerEvent& event) {
    switch (event.type) {
        case ScannerEventType::ChannelConfigChanged:
            if (event.channelConfig) broadcast(json{{"type", "ChannelConfig"}, {"data", protocol::channelConfigToJson(*event.channelConfig)}});
            break;
        case ScannerEventType::ChannelStatusChanged:
            if (event.channelStatus) broadcast(json{{"type", "ChannelStatus"}, {"data", protocol::channelStatusToJson(*event.channelStatus)}});
            break;
        case ScannerEventType::ScanWindowStart:
            broadcast(json{{"type", "ScanWindowStart"}, {"data", {{"id", event.windowId.value_or("")}, {"rxId", event.receiverId.value_or("")}}}});
            break;
        case ScannerEventType::ScanWindowDone:
            broadcast(json{{"type", "ScanWindowDone"}, {"data", {{"id", event.windowId.value_or("")}}}});
            break;
        case ScannerEventType::ScanWindowConfigsChanged:
            broadcast(json{{"type", "ScanWindowConfigsChanged"}});
            break;
    }
}

void HttpServer::broadcast(const json& msg) {
    std::vector<std::shared_ptr<WsClient>> clientsCopy;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clientsCopy = clients_;
    }
    std::string payload = msg.dump();
    std::vector<std::shared_ptr<WsClient>> dead;
    for (auto& c : clientsCopy) {
        std::lock_guard<std::mutex> lock(c->writeMutex);
        boost::system::error_code ec;
        c->ws->text(true);
        c->ws->write(boost::asio::buffer(payload), ec);
        if (ec) dead.push_back(c);
    }
    if (!dead.empty()) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& d : dead) {
            clients_.erase(std::remove(clients_.begin(), clients_.end(), d), clients_.end());
        }
    }
}

} // namespace sdrscan
