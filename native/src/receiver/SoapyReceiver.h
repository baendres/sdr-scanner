#pragma once

#include <gnuradio/soapy/source.h>
#include <gnuradio/top_block.h>
#include <gnuradio/blocks/selector.h>
#include <gnuradio/blocks/null_sink.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../config/Types.h"
#include "../dsp/ScanWindow.h"
#include "../audio/AudioMixer.h"

namespace sdrscan {

// Owns one physical (or Soapy-virtual) receiver: its GNU Radio source block, its own
// gr::top_block, and its own set of ScanWindow objects (each with their own live
// ChannelBlockBase DSP instances). Direct port of the combination of Receiver.py's
// ReceiverBlock/Receiver classes and _runAsProcess - collapsed into one class since we no
// longer need a separate OS process per receiver (see native/README.md Architecture
// Overview).
//
// Every logical Channel appears once per receiver (each receiver needs its own copy of the
// DSP blocks, since a GNU Radio block instance can't be shared across flowgraphs/threads).
// Scanner is responsible for fanning "hot" control updates (mute, squelch, ...) out to every
// receiver's copy via withChannel().
//
// Window hopping architecture: every configured scan window's DSP chain is built and wired
// into ONE persistent flowgraph at once (source_ -> rfSelector_ -> each window's block ->
// audioSelector_ -> audioSink_), rather than rebuilding a flowgraph per hop. Hopping is then
// just flipping both selectors' live index (gr::blocks::selector::set_input_index/
// set_output_index, safe to call while running - the same mechanism squelch/gain live
// updates already use), with zero GNU Radio topology change and zero USB stream
// re-negotiation on the hardware source. Ported from Receiver.py's post-fork
// "one large block with all windows and selectors" rework (upstream commit 54a5ac4) - a
// cleaner solution to the same problem an earlier version of this class solved with a custom
// RfRingBuffer bridging two separate flowgraphs; that approach is gone now that hopping never
// touches the flowgraph containing the hardware source at all.
//
// The one thing that *does* still require stopping/restarting the whole flowgraph (hardware
// included) is a structural change to the window set itself (a channel/receiver add, remove,
// or edit) - rare compared to hopping, so paying the USB re-negotiation cost there is a fine
// trade.
class SoapyReceiver {
public:
    SoapyReceiver(ReceiverConfig config,
                  std::function<void(ChannelStatusUpdate)> statusCallback,
                  std::shared_ptr<AudioRingBufferSinkBlock> audioSink);

    const std::string& id() const { return config_.id; }
    const ReceiverConfig& config() const { return config_; }

    std::vector<int> getSampleRates();

    // Enumerates connected SDR hardware across every installed SoapySDR driver module
    // (RTL-SDR, HackRF, LimeSDR, ...) - each entry is a device's raw SoapySDR args (driver,
    // label, serial, etc, whatever that driver module reports). Used by the settings page's
    // "Scan for Receivers" button so a device can be discovered and added without knowing
    // SoapySDR device-arg syntax up front. Static: a pure hardware query, no receiver instance
    // needed. Never throws - a failing/misbehaving driver module is logged and skipped rather
    // than taking down the whole scan (and the caller, e.g. the HTTP request handling it).
    static std::vector<std::map<std::string, std::string>> scanAvailableDevices();

    // Thread-safe: queues new scan window configs for this receiver's run() loop to pick up.
    // Rebuilding scan windows is a flowgraph topology change, so it's confined to the thread
    // that owns this receiver's top_block (this is the "structural update" tier - see
    // native/README.md).
    void postScanWindowConfigs(std::vector<ScanWindowConfig> configs);

    // Blocks until stop() is called from another thread. Meant to be a thread entry point.
    //   nextWindowIdProvider: asked for a window id whenever this receiver goes idle
    //                         (Scanner's round-robin scheduler); "" means none available yet.
    //   onWindowStart/onWindowDone: notified of transitions, for status broadcast.
    void run(const std::function<std::string()>& nextWindowIdProvider,
             const std::function<void(const std::string& windowId)>& onWindowStart,
             const std::function<void(const std::string& windowId)>& onWindowDone);
    void stop();

    // Safe to call from any thread - GNU Radio block parameter setters are designed to be
    // called live while the flowgraph runs (the same mechanism GRC's live GUI widgets use).
    // No-op if this receiver doesn't currently have a window containing that channel.
    void withChannel(const std::string& channelId, const std::function<void(ChannelBlockBase&)>& fn);

private:
    std::shared_ptr<ScanWindow> buildWindow(const ScanWindowConfig& cfg);
    void applyPendingConfigsIfAny();
    // Tears down and rebuilds the entire flowgraph (source_ persists; everything downstream of
    // it is fresh) around the given window set. Safe to call with an empty vector (leaves the
    // graph torn down and unbuilt - run()'s loop just idles until real configs arrive).
    void rebuildGraph(std::vector<ScanWindowConfig> configs);
    void ensureRunning();
    void shutdownGraph();
    bool startWindow(const std::string& windowId); // false if the window vanished (config race) or a hardware error occurred
    void stopCurrentWindow();
    void checkCurrentWindow();

    ReceiverConfig config_;
    std::function<void(ChannelStatusUpdate)> statusCallback_;
    std::shared_ptr<AudioRingBufferSinkBlock> audioSink_;

    gr::soapy::source::sptr source_;
    std::optional<std::vector<int>> cachedSampleRates_;
    int lastSetSampleRate_ = 0; // avoids a ~100ms hardware sample-rate reset on every hop when consecutive windows share a rate

    // Rebuilt from scratch (a fresh top_block, discarding the old one) on every structural
    // change - see the class comment. Null/unbuilt whenever no windows are configured yet.
    gr::top_block_sptr receiverTopBlock_;
    gr::blocks::selector::sptr rfSelector_;    // 1 input (source_), N+1 outputs (one per window + discard)
    gr::blocks::selector::sptr audioSelector_; // N inputs (one per window), 1 output (audioSink_)
    gr::blocks::null_sink::sptr rfDiscardSink_;
    int rfDiscardPortIndex_ = -1;
    bool graphBuilt_ = false;
    bool graphRunning_ = false;

    std::mutex mailboxMutex_;
    std::optional<std::vector<ScanWindowConfig>> pendingConfigs_;

    std::mutex windowsMutex_; // guards scanWindowsById_/scanWindowIndexById_ (read by withChannel from any thread)
    std::unordered_map<std::string, std::shared_ptr<ScanWindow>> scanWindowsById_;
    std::unordered_map<std::string, int> scanWindowIndexById_; // window id -> selector port index

    std::shared_ptr<ScanWindow> currentWindow_;
    double windowTimeout_ = 0.0;
    bool windowRunning_ = false;

    // A hardware/USB communication failure (e.g. a flaky I2C write to the tuner) surfaces as
    // an exception from startWindow()'s SoapySDR calls - caught there so it can't escape this
    // receiver's thread uncaught (which would abort the whole process). If the device is
    // genuinely offline, every window on this receiver would fail in turn; this cooldown keeps
    // that from becoming a tight retry loop hammering the device and spamming the log.
    double nextStartAttemptAllowedAt_ = 0.0;

    std::atomic<bool> stopFlag_{false};
};

} // namespace sdrscan
