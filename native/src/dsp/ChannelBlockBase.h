#pragma once

#include <gnuradio/hier_block2.h>
#include <gnuradio/blocks/mute.h>
#include <gnuradio/gr_complex.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "../config/Types.h"
#include "HelperBlocks.h"

namespace sdrscan {

inline double dbToRatio(double dB) { return std::pow(10.0, dB / 20.0); }

// Splits a decimation ratio into {stage1, stage2} for two-stage input channelization (see the
// "two-stage channelization" note in native/README.md): stage2 is the smallest factor of
// totalDecim that's >= minStage2 (so the sharp/narrow channel-select filter runs at as low a
// rate as possible), and stage1 absorbs the rest via a cheap wide-transition filter at the full
// RF rate. Returns {totalDecim, 1} (i.e. "don't split") if no such factor exists - already a
// modest enough ratio that a single sharp stage is cheap on its own, and splitting further
// would just add a useless extra block with no real transition-width room to exploit.
inline std::pair<int, int> splitDecimation(int totalDecim, int minStage2) {
    for (int d2 = minStage2; d2 <= totalDecim; d2++) {
        if (totalDecim % d2 == 0) {
            return {totalDecim / d2, d2};
        }
    }
    return {totalDecim, 1};
}

// C++ port of Channel.py's ChannelBlock_Base. A gr::hier_block2 taking a complex baseband
// stream in (already tuned/decimated by the ScanWindow) and producing a float audio stream
// out. Concrete demod modes (ChannelBlockFM, ChannelBlockAM) extend this.
//
// All of the "hot update" setters (setSquelchValue, setAudioGain, setMute, ...) are safe to
// call while the flowgraph is running - this is exactly the mechanism the control API uses to
// apply live changes with no flowgraph restart (see native/README.md).
class ChannelBlockBase : public gr::hier_block2 {
public:
    ChannelBlockBase(const std::string& channelId,
                      const std::string& label,
                      bool mute,
                      TriBool solo,
                      bool hold,
                      double squelchThreshold,
                      double audioGain_dB,
                      double dwellTime_s,
                      int audioSampleRate,
                      std::optional<double> squelchNoiseMargin_dB,
                      std::function<void(ChannelStatusUpdate)> statusCallback);

    const std::string& id() const { return channelId_; }

    virtual double getMinimumScanTime() const { return 0.1; }

    void setMute(bool mute);
    void setSolo(TriBool solo);
    void setHold(bool hold);
    void setDwellTime(double dwellTime_s) { dwellTime_s_ = dwellTime_s; }

    virtual void setForceActive(bool forceActive) = 0;
    virtual void setSquelchValue(double squelchThreshold) = 0;
    virtual void setAudioGain(double audioGain_dB) = 0;
    // No-op by default; only ChannelBlockFM supports CTCSS (it's an FM sub-audible-tone
    // scheme - AM/SSB/etc channel blocks don't have a meaningful implementation).
    virtual void setCtcssTone(std::optional<double> /*toneHz*/) {}

    // FM noise squelch: no-op by default; only ChannelBlockFM implements it (FM's capture
    // effect - AM has no equivalent). See ChannelBlockFM's header note.
    virtual void setNoiseSquelchThreshold(std::optional<double> /*thresholdDb*/) {}

    // Adaptive ("noise-relative") squelch: when set, the channel's effective squelch threshold
    // tracks the live noise floor estimate (noiseFloor_dBFS_) plus this margin instead of a
    // fixed absolute squelchThreshold_ - keeps squelch correctly calibrated as band conditions
    // change (interference, time of day, ...) instead of needing to be re-tuned by hand as
    // noise increases. Unset (the default) keeps today's fixed-threshold behavior. No-op by
    // default; ChannelBlockFM/AM override to push a fresh threshold to their own squelch block
    // whenever the noise floor estimate updates (see onNoiseFloorUpdated()).
    virtual void setSquelchNoiseMargin(std::optional<double> /*marginDb*/) {}

    // Recomputes + reports (if changed) the channel's status; must be called periodically
    // (the Scanner polls this while a ScanWindow is running).
    virtual ChannelStatus getStatus() = 0;

    // Latest telemetry, as last reported via updateRSSI()/updateVolume(). Public so
    // ChannelBlockEAS can surface its internal ChannelBlockFM's telemetry as its own (see
    // ChannelBlockEAS::getStatus()) - that FM block's own getStatus()/computeAndReportStatus()
    // is never called (EAS has its own trigger-latch status model), so this is otherwise the
    // only way to reach it.
    std::optional<float> rssi() const { return rssi_dBFS_; }
    std::optional<float> noiseFloor() const { return noiseFloor_dBFS_; }
    std::optional<float> volume() const { return volume_dBFS_; }

protected:
    // Common status bookkeeping shared by squelch-gated demod modes: given whether the
    // squelch(es) are currently unmuted, tracks active/dwell timing and reports the resulting
    // status via reportStatus(). Not used by ChannelBlockEAS, which has its own trigger-latch
    // active/dwell model but still calls reportStatus() directly to share the same
    // change/periodic reporting logic.
    ChannelStatus computeAndReportStatus(bool unmutedNow);

    // Reports `status` via statusCallback_ if it changed since the last report, or periodically
    // while non-idle (throttled to STATUS_UPDATE_TIME_S) - same throttling the Python version
    // used. Returns `status` unchanged, so callers can `return reportStatus(status);`.
    ChannelStatus reportStatus(ChannelStatus status);

    void connectVolume(const gr::basic_block_sptr& sourceBlock, int sourceBlockPort);

    void updateRSSI(float dBFS);
    void updateVolume(float dBFS);

    // Returns the threshold that should currently apply: noiseFloor_dBFS_ + margin if adaptive
    // squelch is on and a noise floor estimate exists yet, else the static squelchThreshold_
    // (also used before the first noise-floor estimate arrives, or whenever adaptive mode is
    // off). FM/AM's setSquelchNoiseMargin overrides read squelchNoiseMargin_dB_ directly rather
    // than through this - it's provided for subclasses that want the resolved value in one call.
    double effectiveSquelchThreshold() const;

    // True only if newThreshold actually differs from the last value this returned true for (or
    // this is the first call) - lets a caller skip a redundant gr block set_threshold() call.
    // getStatus() (and therefore refreshAdaptiveSquelchThreshold()) runs on SoapyReceiver's
    // ~1ms window-scheduling loop (see checkCurrentWindow()), far faster than the noise floor
    // estimate itself can change (RSSI_UPDATE_FREQ_HZ, 4Hz) - without this, an adaptive-squelch
    // channel would call into a live GNU Radio block's setter roughly 1000x more often than the
    // threshold could possibly have moved. Real-hardware testing found this was enough overhead,
    // multiplied across every adaptive-squelch channel in the active window, to make the whole
    // receiver's audio production fall behind real time.
    bool adaptiveThresholdChanged(double newThreshold);

    // Time-based debounce: filters brief noise spikes from being treated as a genuine
    // open/close by requiring the raw squelch-open decision to persist for
    // SQUELCH_DEBOUNCE_SECONDS before the reported/gated state follows it. Call once per
    // getStatus() with the raw (undebounced) decision; the return value is what should
    // actually drive both the real audio gate and computeAndReportStatus().
    bool debounceSquelch(bool rawUnmuted);

    // Same "skip the call unless the value actually changed" reasoning as
    // adaptiveThresholdChanged(), applied to the inline audio gate: getStatus() recomputes the
    // debounced mute decision every call (~1kHz per channel, see adaptiveThresholdChanged()'s
    // comment), but real-hardware testing found gate->set_mute() was being called that often
    // regardless of whether the decision actually flipped - same live-block-setter overhead,
    // multiplied across every channel in the active window.
    // Returns true if it actually pushed (i.e. muted differed from the last call), matching
    // adaptiveThresholdChanged()'s return convention.
    bool setAudioGateMuted(const gr::blocks::mute_ff::sptr& gate, bool muted);

    std::string channelId_;
    std::string label_;
    bool mute_;
    TriBool solo_;
    bool hold_;
    bool forceActive_ = false;
    double squelchThreshold_;
    std::optional<double> squelchNoiseMargin_dB_;
    double audioGainFactor_;
    double dwellTime_s_;
    int audioSampleRate_;

    bool debounceRawUnmuted_ = false;
    double debounceRawChangedAt_ = 0.0;
    bool debounceStableUnmuted_ = false;

    std::optional<double> lastPushedAdaptiveThreshold_;
    std::optional<bool> lastAudioGateMuted_;

    bool active_ = false;
    double lastActive_ = 0.0;
    std::optional<ChannelStatus> lastStatusReport_;
    double lastStatusTime_ = 0.0;

    std::optional<float> rssi_dBFS_;
    std::optional<float> noiseFloor_dBFS_;
    std::optional<float> volume_dBFS_;

    std::function<void(ChannelStatusUpdate)> statusCallback_;

    gr::blocks::mute_ff::sptr blockAudioMute_;
    std::shared_ptr<MagToPowerLowPassBlock> blockVolume_;
};

} // namespace sdrscan
