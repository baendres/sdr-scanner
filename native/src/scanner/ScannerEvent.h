#pragma once

#include <optional>
#include <string>

#include "../config/Types.h"

namespace sdrscan {

// Replaces Python's queue-based Scanner->UI message bus (sendScannerMsg / addOutputQueue):
// Scanner emits these to a single callback (HttpServer subscribes and fans them out over
// WebSocket, mirroring web_gui.py's broadcaster).
enum class ScannerEventType {
    ChannelConfigChanged,
    ChannelStatusChanged,
    ScanWindowStart,
    ScanWindowDone,
    ScanWindowConfigsChanged,
};

struct ScannerEvent {
    ScannerEventType type;
    std::optional<ChannelConfig> channelConfig;
    std::optional<ChannelStatusUpdate> channelStatus;
    std::optional<std::string> windowId;
    std::optional<std::string> receiverId;
};

} // namespace sdrscan
