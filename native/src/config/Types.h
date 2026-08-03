#pragma once

#include <optional>
#include <string>
#include <map>
#include <cstdint>

namespace sdrscan {

// A channel's demodulation mode. SSB is a deferred follow-up (see README) - the enum leaves
// room for it but it's not implemented yet.
enum class ChannelMode {
    FM,
    NFM,
    AM,
    NOAA,     // NOAA weather radio SAME/EAS attention tone (1050 Hz) on narrowband FM
    BFM_EAS,  // Broadcast EAS two-tone attention signal (853/960 Hz) on wideband FM
    DMR,      // Conventional (non-trunked) DMR, one timeslot per ChannelConfig - see dmrSlot
};

std::string channelModeToString(ChannelMode mode);
std::optional<ChannelMode> channelModeFromString(const std::string& s);

enum class ReceiverType {
    RTL_SDR,
    SOAPY,
};

enum class ChannelStatus {
    IDLE = 0,
    ACTIVE = 1,
    DWELL = 2,
    HOLD = 3,
    FORCE_ACTIVE = 4,
};

std::string channelStatusToString(ChannelStatus status);

// Emitted by a live Channel whenever its status (or periodically, its RSSI/volume) changes.
// Replaces the Python `statusPipe.send(...)` mechanism - here it's a direct callback since
// everything runs in one process.
struct ChannelStatusUpdate {
    std::string channelId;
    ChannelStatus status = ChannelStatus::IDLE;
    std::optional<float> rssi_dBFS;
    std::optional<float> noiseFloor_dBFS;
    std::optional<float> volume_dBFS;
    // FM/NFM only (see ChannelBlockFM's noise squelch chain) - unset for AM/EAS-without-FM-
    // telemetry-copy or before the reference band's first measurement.
    std::optional<float> noiseRefLevel_dBFS;
};

std::string receiverTypeToString(ReceiverType type);
std::optional<ReceiverType> receiverTypeFromString(const std::string& s);

// Tri-state solo, matching the Python semantics:
//   Unset    -> solo inactive, use `mute`
//   true     -> this channel is soloed (audible)
//   false    -> some other channel is soloed, this one is muted-by-solo
using TriBool = std::optional<bool>;

struct ChannelConfig {
    std::string id;                 // uuid string
    int64_t freq_hz = 0;
    std::string label;
    ChannelMode mode = ChannelMode::FM;

    double audioGain_dB = 0.0;
    double dwellTime_s = 3.0;
    double squelchThreshold = -55.0;
    std::optional<double> ctcssToneHz;   // unset = power-squelch only
    // Adaptive ("noise-relative") squelch: when set, overrides squelchThreshold - the channel's
    // effective threshold instead tracks its live noise floor estimate plus this margin. Unset
    // = today's fixed-threshold behavior. See ChannelBlockBase::effectiveSquelchThreshold().
    std::optional<double> squelchNoiseMargin_dB;
    // FM noise squelch: when set, the channel also requires a reference-band (above voice,
    // where FM's capture effect suppresses hiss once a real signal captures the receiver)
    // power level below this threshold before it's considered open - rejects broadband noise
    // impulses that pass the power squelch and persist long enough to clear debounce, which
    // pure duration-based squelch can't distinguish from a real short transmission. FM/NFM
    // only (no capture effect on AM); unset = today's power-squelch-only behavior. See
    // ChannelBlockFM's noise squelch chain.
    std::optional<double> noiseSquelchThreshold_dB;

    // DMR only. DMR repeaters are dual-timeslot (TS1/TS2 can carry two independent simultaneous
    // calls on one frequency) - each timeslot is its own ChannelConfig row at the same freq_hz,
    // with its own mute/hold/status, sharing one underlying C4FM demod + DSDcc decoder instance
    // (see ScanWindow::buildChannelBlock()). 1 or 2; unset is invalid for ChannelMode::DMR.
    std::optional<int> dmrSlot;
    // DMR only. Unset = unmute for any talkgroup heard on this slot. When set, audio only
    // unmutes for calls addressed to this talkgroup ID. NOTE: upstream DSDcc's DSDDMR only
    // exposes the decoded target/source address as a formatted human-readable string
    // (getSlot0Text()/getSlot1Text()), not a queryable numeric field (m_slot1Addresses.m_target
    // etc. are private) - same "needs a small vendored patch" situation as
    // DSDDecoder::getOpts()/getState() (see native/README.md). ChannelBlockDMR parses the
    // talkgroup number back out of that text as a stopgap; a patch to expose it directly would
    // be more robust if DSDcc gets bumped later.
    std::optional<uint32_t> dmrTalkgroupFilter;

    bool enabled = true;
    std::optional<double> disableUntil; // unix time
    bool mute = false;
    TriBool solo;
    bool hold = false;
    bool forceActive = false;           // runtime-only, not persisted

    int sortOrder = 0;

    bool isEnabledNow(double nowUnixTime) const {
        if (disableUntil.has_value()) {
            return nowUnixTime > *disableUntil; // caller should also clear disableUntil in this case
        }
        return enabled;
    }
};

struct ReceiverConfig {
    std::string id;
    ReceiverType type = ReceiverType::RTL_SDR;
    std::optional<std::string> deviceArg;
    std::optional<std::string> driver;      // required for SOAPY
    std::optional<double> gain;
    std::map<std::string, double> gains;    // per-stage gains, e.g. LNA/MIX/VGA
    bool enabled = true;
    int sortOrder = 0;
};

struct OutputConfig {
    int64_t id = 0;
    std::string type;          // local | udp | websocket | icecast
    std::string configJson;    // type-specific fields, e.g. {"serverIp":"...","serverPort":...}
    bool enabled = true;
};

struct ScannerSettings {
    int maxChannelsPerWindow = 16;
    std::string httpHost = "0.0.0.0";
    int httpPort = 8080;
};

} // namespace sdrscan
