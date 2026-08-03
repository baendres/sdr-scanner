#pragma once

#include <gnuradio/hier_block2.h>
#include <gnuradio/blocks/add_blk.h>
#include <gnuradio/filter/rational_resampler.h>

#include <memory>
#include <string>
#include <vector>

#include "../config/Types.h"
#include "ChannelBlockBase.h"

namespace sdrscan {

// A group of Channels that all fit within one receiver's instantaneous bandwidth, tuned
// around a shared hardware center frequency. Direct port of ScanWindow.py.
struct ScanWindowConfig {
    std::string id;
    int64_t hardwareFreq_hz = 0;
    int64_t rfBandwidth = 0; // Hz - receivers pick a samplerate >= this to cover the window
    std::vector<ChannelConfig> channelConfigs;
};

// GNU Radio block that gets 'plugged in' between a receiver's source and the audio mixer
// while this window is being scanned: mixes all of its Channels' demodulated audio together.
class ScanWindowBlock : public gr::hier_block2 {
public:
    ScanWindowBlock(const std::vector<std::shared_ptr<ChannelBlockBase>>& channels, int audioSampleRate, int globalAudioSampleRate);

private:
    gr::blocks::add_ff::sptr mixerAdd_;
    gr::filter::rational_resampler_fff::sptr resampler_; // null if audioSampleRate == globalAudioSampleRate
};

// Runtime (receiver-side) instantiation of a ScanWindowConfig - owns the live ChannelBlocks.
class ScanWindow {
public:
    ScanWindow(const ScanWindowConfig& config,
               int rfSampleRate,
               std::function<void(ChannelStatusUpdate)> statusCallback);

    const std::string& id() const { return id_; }
    int64_t hardwareFreq_hz() const { return hardwareFreq_hz_; }
    int rfSampleRate() const { return rfSampleRate_; }
    int audioSampleRate() const { return audioSampleRate_; }

    gr::basic_block_sptr block() const { return scanWindowBlock_; }
    const std::vector<std::shared_ptr<ChannelBlockBase>>& channels() const { return channels_; }
    std::shared_ptr<ChannelBlockBase> channelById(const std::string& channelId) const;

    // true if any channel is non-idle; polls every channel's getStatus() (which also fires
    // the status callback on change, mirroring Channel.py's getStatus(statusPipe) pattern).
    bool isActive();

    // Minimum time this window needs to stay tuned (e.g. for detectors that need to build up
    // enough samples) - currently always the base 0.1s floor since NOAA/EAS modes (which had a
    // higher floor in Python) are deferred.
    double getMinimumScanTime() const;

    // Picks an RF samplerate from `availableRates` sufficient to cover the window's
    // rfBandwidth, and a resulting audio samplerate (decimates cleanly to the global
    // AUDIO_SAMPLERATE if possible, otherwise the closest rate that divides evenly).
    // requireDmrCompatibleRate: pass true if any channel in this window is ChannelMode::DMR -
    // restricts candidates to whole multiples of DMR_DISCRIMINATOR_RATE_HZ (see
    // ChannelBlockDMR.cpp), since a rate that doesn't divide evenly makes its constructor throw.
    static int selectRfSampleRate(const std::vector<int>& availableRates, int64_t rfBandwidth,
                                   bool requireDmrCompatibleRate);
    static int selectAudioSampleRate(int rfSampleRate);

private:
    std::string id_;
    int64_t hardwareFreq_hz_;
    int rfSampleRate_;
    int audioSampleRate_;

    std::vector<std::shared_ptr<ChannelBlockBase>> channels_;
    std::shared_ptr<ScanWindowBlock> scanWindowBlock_;

    mutable std::optional<double> minimumScanTime_;
};

} // namespace sdrscan
