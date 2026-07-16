#include "HttpServer.h"
#include "Protocol.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

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

std::string channelIdFromPath(const std::string& target, const std::string& prefix) {
    if (target.size() <= prefix.size() || target.compare(0, prefix.size(), prefix) != 0) return "";
    return target.substr(prefix.size());
}

} // namespace

HttpServer::HttpServer(Scanner& scanner, std::string host, int port, std::string webRoot)
    : scanner_(scanner), host_(std::move(host)), port_(port), webRoot_(std::move(webRoot)) {
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

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& c : clients_) {
        boost::system::error_code ec;
        c->ws->close(websocket::close_code::normal, ec);
    }
    clients_.clear();
}

void HttpServer::acceptLoop() {
    while (!stopFlag_) {
        boost::system::error_code ec;
        tcp::socket socket(ioc_);
        acceptor_->accept(socket, ec);
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
    if (body.contains("squelchThreshold")) scanner_.setChannelSquelch(channelId, body.at("squelchThreshold").get<double>());
    if (body.contains("ctcssToneHz")) {
        auto v = body.at("ctcssToneHz");
        scanner_.setChannelCtcssTone(channelId, v.is_null() ? std::nullopt : std::optional<double>(v.get<double>()));
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
        if (type == "ChannelMute") {
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
