#include "Types.h"

#include <algorithm>

namespace sdrscan {

namespace {
std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}
} // namespace

std::string channelModeToString(ChannelMode mode) {
    switch (mode) {
        case ChannelMode::FM: return "FM";
        case ChannelMode::NFM: return "NFM";
        case ChannelMode::AM: return "AM";
        case ChannelMode::NOAA: return "NOAA";
        case ChannelMode::BFM_EAS: return "BFM_EAS";
    }
    return "FM";
}

std::optional<ChannelMode> channelModeFromString(const std::string& s) {
    std::string u = toUpper(s);
    if (u == "FM") return ChannelMode::FM;
    if (u == "NFM") return ChannelMode::NFM;
    if (u == "AM") return ChannelMode::AM;
    if (u == "NOAA") return ChannelMode::NOAA;
    if (u == "BFM_EAS") return ChannelMode::BFM_EAS;
    return std::nullopt;
}

std::string receiverTypeToString(ReceiverType type) {
    switch (type) {
        case ReceiverType::RTL_SDR: return "RTL-SDR";
        case ReceiverType::SOAPY: return "SOAPY";
    }
    return "RTL-SDR";
}

std::optional<ReceiverType> receiverTypeFromString(const std::string& s) {
    std::string u = toUpper(s);
    if (u == "RTL-SDR" || u == "RTL_SDR" || u == "RTLSDR") return ReceiverType::RTL_SDR;
    if (u == "SOAPY") return ReceiverType::SOAPY;
    return std::nullopt;
}

std::string channelStatusToString(ChannelStatus status) {
    switch (status) {
        case ChannelStatus::IDLE: return "IDLE";
        case ChannelStatus::ACTIVE: return "ACTIVE";
        case ChannelStatus::DWELL: return "DWELL";
        case ChannelStatus::HOLD: return "HOLD";
        case ChannelStatus::FORCE_ACTIVE: return "FORCE_ACTIVE";
    }
    return "IDLE";
}

} // namespace sdrscan
