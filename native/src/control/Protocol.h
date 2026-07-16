#pragma once

#include <nlohmann/json.hpp>

#include "../config/Types.h"
#include "../scanner/Scanner.h"

// JSON wire format for the REST/WebSocket control API. Field names deliberately match
// Channel.py's getJson() where possible, so the ported web UI (native/web/) needed only
// additive changes (squelch/CTCSS controls), not a rewrite.
namespace sdrscan::protocol {

using json = nlohmann::json;

inline json triBoolToJson(TriBool v) { return v.has_value() ? json(*v) : json(nullptr); }

inline TriBool jsonToTriBool(const json& j) {
    if (j.is_null()) return std::nullopt;
    return j.get<bool>();
}

inline json channelConfigToJson(const ChannelConfig& cc) {
    return json{
        {"id", cc.id},
        {"freq_hz", cc.freq_hz},
        {"label", cc.label},
        {"mode", channelModeToString(cc.mode)},
        {"dwellTime_s", cc.dwellTime_s},
        {"audioGain_dB", cc.audioGain_dB},
        {"squelchThreshold", cc.squelchThreshold},
        {"ctcssToneHz", cc.ctcssToneHz.has_value() ? json(*cc.ctcssToneHz) : json(nullptr)},
        {"enabled", cc.enabled},
        {"disableUntil", cc.disableUntil.has_value() ? json(*cc.disableUntil) : json(nullptr)},
        {"mute", cc.mute},
        {"solo", triBoolToJson(cc.solo)},
        {"hold", cc.hold},
        {"forceActive", cc.forceActive},
    };
}

inline json channelStatusToJson(const ChannelStatusUpdate& u) {
    return json{
        {"id", u.channelId},
        {"status", channelStatusToString(u.status)},
        {"rssi", u.rssi_dBFS.has_value() ? json(*u.rssi_dBFS) : json(nullptr)},
        {"noiseFloor", u.noiseFloor_dBFS.has_value() ? json(*u.noiseFloor_dBFS) : json(nullptr)},
        {"volume", u.volume_dBFS.has_value() ? json(*u.volume_dBFS) : json(nullptr)},
    };
}

inline json receiverConfigToJson(const ReceiverConfig& rc) {
    json gains = json::object();
    for (const auto& [k, v] : rc.gains) gains[k] = v;
    return json{
        {"id", rc.id},
        {"type", receiverTypeToString(rc.type)},
        {"deviceArg", rc.deviceArg.has_value() ? json(*rc.deviceArg) : json(nullptr)},
        {"driver", rc.driver.has_value() ? json(*rc.driver) : json(nullptr)},
        {"gain", rc.gain.has_value() ? json(*rc.gain) : json(nullptr)},
        {"gains", gains},
        {"enabled", rc.enabled},
    };
}

inline json outputConfigToJson(const OutputConfig& oc) {
    json config = json::object();
    try {
        config = json::parse(oc.configJson);
    } catch (const json::exception&) {
    }
    return json{
        {"id", oc.id},
        {"type", oc.type},
        {"config", config},
        {"enabled", oc.enabled},
    };
}

inline json snapshotToJson(const ScannerSnapshot& s) {
    json channels = json::array();
    for (const auto& cc : s.channels) channels.push_back(channelConfigToJson(cc));
    json receivers = json::array();
    for (const auto& rc : s.receivers) receivers.push_back(receiverConfigToJson(rc));
    json outputs = json::array();
    for (const auto& oc : s.outputs) outputs.push_back(outputConfigToJson(oc));
    json statuses = json::array();
    for (const auto& st : s.channelStatuses) statuses.push_back(channelStatusToJson(st));

    return json{
        {"scanner", json{{"maxChannelsPerWindow", s.settings.maxChannelsPerWindow}}},
        {"channels", channels},
        {"receivers", receivers},
        {"outputs", outputs},
        {"channelStatuses", statuses},
    };
}

// Builds a ChannelConfig from a POST /api/channels body. Throws std::runtime_error /
// nlohmann::json::exception on invalid input (caller turns that into a 400 response).
inline ChannelConfig channelConfigFromJson(const json& j) {
    ChannelConfig cc;
    cc.freq_hz = j.at("freq_hz").get<int64_t>();
    cc.label = j.value("label", std::to_string(cc.freq_hz));
    if (j.contains("mode")) {
        auto mode = channelModeFromString(j.at("mode").get<std::string>());
        if (!mode) throw std::runtime_error("Unknown channel mode");
        cc.mode = *mode;
    }
    cc.audioGain_dB = j.value("audioGain_dB", 0.0);
    cc.dwellTime_s = j.value("dwellTime_s", 3.0);
    cc.squelchThreshold = j.value("squelchThreshold", -55.0);
    if (j.contains("ctcssToneHz") && !j.at("ctcssToneHz").is_null()) {
        cc.ctcssToneHz = j.at("ctcssToneHz").get<double>();
    }
    return cc;
}

} // namespace sdrscan::protocol
