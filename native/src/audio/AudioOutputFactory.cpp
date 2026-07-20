#include "AudioOutput.h"
#include "AudioOutputLocal.h"
#include "AudioOutputUdp.h"
#include "AudioOutputWebsocket.h"
#include "AudioOutputIcecast.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace sdrscan {

std::shared_ptr<AudioOutput> createAudioOutput(const OutputConfig& cfg) {
    using json = nlohmann::json;

    std::string type = cfg.type;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });

    json j = json::object();
    if (!cfg.configJson.empty()) {
        try {
            j = json::parse(cfg.configJson);
        } catch (const json::exception&) {
            // malformed config_json - fall back to defaults below
        }
    }

    if (type == "local") {
        return std::make_shared<AudioOutputLocal>();
    }
    if (type == "udp") {
        std::string serverIp = j.value("serverIp", "127.0.0.1");
        int serverPort = j.value("serverPort", 12345);
        return std::make_shared<AudioOutputUdp>(serverIp, serverPort);
    }
    if (type == "websocket") {
        std::string host = j.value("host", "0.0.0.0");
        int port = j.value("port", 8123);
        return std::make_shared<AudioOutputWebsocket>(host, port);
    }
    if (type == "icecast") {
        std::string url = j.at("url").get<std::string>(); // required - no sane default
        std::string password = j.value("password", "");
        return std::make_shared<AudioOutputIcecast>(url, password);
    }

    throw std::runtime_error("Unknown audio output type: " + cfg.type);
}

} // namespace sdrscan
