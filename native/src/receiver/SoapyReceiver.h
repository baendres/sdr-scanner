#pragma once

#include <gnuradio/soapy/source.h>
#include <gnuradio/top_block.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../config/Types.h"
#include "../dsp/ScanWindow.h"
#include "../audio/AudioMixer.h"
#include "RfRingBuffer.h"

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
class SoapyReceiver {
public:
    SoapyReceiver(ReceiverConfig config,
                  std::function<void(ChannelStatusUpdate)> statusCallback,
                  std::shared_ptr<AudioRingBufferSinkBlock> audioSink);

    const std::string& id() const { return config_.id; }
    const ReceiverConfig& config() const { return config_; }

    std::vector<int> getSampleRates();

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
    bool startWindow(const std::string& windowId); // false if the window vanished (config race)
    void stopCurrentWindow();
    void checkCurrentWindow();

    ReceiverConfig config_;
    std::function<void(ChannelStatusUpdate)> statusCallback_;
    std::shared_ptr<AudioRingBufferSinkBlock> audioSink_;

    gr::soapy::source::sptr source_;
    std::optional<std::vector<int>> cachedSampleRates_;

    // Two separate flowgraphs, bridged through an RfRingBuffer (see RfRingBuffer.h for the
    // full "why" - short version: stopping/restarting *any* flowgraph containing the hardware
    // source, even via topBlock_->lock()/unlock(), forces GNU Radio to stop+restart every
    // block in it including the source, which forces the driver to fully re-negotiate the USB
    // stream. Splitting into two flowgraphs means the one with the hardware source never gets
    // touched again after its first start(), no matter how often the processing side gets
    // reconfigured for a window hop).
    //
    // captureTopBlock_: source_ -> rfRingBufferSink_. Started exactly once, never stopped
    // again until receiver shutdown.
    gr::top_block_sptr captureTopBlock_;
    std::shared_ptr<RfRingBuffer> rfRingBuffer_;
    std::shared_ptr<RfRingBufferSinkBlock> rfRingBufferSink_;
    bool captureStarted_ = false;

    // windowTopBlock_: rfRingBufferSource_ -> current window's block -> audioSink_. Freely
    // stopped/reconfigured/restarted on every window hop - cheap, since it touches no
    // hardware at all.
    gr::top_block_sptr windowTopBlock_;
    std::shared_ptr<RfRingBufferSourceBlock> rfRingBufferSource_;

    std::mutex mailboxMutex_;
    std::optional<std::vector<ScanWindowConfig>> pendingConfigs_;

    std::mutex windowsMutex_; // guards scanWindowsById_ (read by withChannel from any thread)
    std::unordered_map<std::string, std::shared_ptr<ScanWindow>> scanWindowsById_;

    std::shared_ptr<ScanWindow> currentWindow_;
    double windowTimeout_ = 0.0;
    bool windowRunning_ = false;

    std::atomic<bool> stopFlag_{false};
};

} // namespace sdrscan
