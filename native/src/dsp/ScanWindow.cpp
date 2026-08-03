#include "ScanWindow.h"
#include "Const.h"
#include "ChannelBlockFM.h"
#include "ChannelBlockAM.h"
#include "ChannelBlockEAS.h"
#include "ChannelBlockDMR.h"
#include "ChannelBlockP25Voice.h"
#include "../util/Uuid.h"

#include <gnuradio/filter/firdes.h>
#include <gnuradio/sptr_magic.h>

#include <algorithm>
#include <map>
#include <stdexcept>

namespace sdrscan {

namespace {

// DMR-only: TS1/TS2 channels at the same freq_hz share one C4FM front end + DSDcc decoder (see
// ChannelBlockDMR's header) - keyed here across the loop in ScanWindow::ScanWindow() so the
// second slot's ChannelBlockDMR can be handed the first's decodeBlock() instead of building its
// own.
using DmrDecodersByFreq = std::map<int64_t, std::shared_ptr<DsdccDecodeBlock>>;

std::shared_ptr<ChannelBlockBase> buildChannelBlock(const ChannelConfig& cc,
                                                      int64_t hardwareFreq_hz,
                                                      int rfSampleRate,
                                                      int audioSampleRate,
                                                      DmrDecodersByFreq& dmrDecodersByFreq,
                                                      std::function<void(ChannelStatusUpdate)> statusCallback) {
    std::shared_ptr<ChannelBlockBase> block;
    switch (cc.mode) {
        case ChannelMode::FM:
        case ChannelMode::NFM: {
            int deviation_hz = (cc.mode == ChannelMode::NFM) ? 2500 : 5000;
            block = gnuradio::make_block_sptr<ChannelBlockFM>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.squelchThreshold, cc.audioGain_dB,
                cc.dwellTime_s, cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate,
                deviation_hz, cc.ctcssToneHz, cc.squelchNoiseMargin_dB,
                cc.noiseSquelchThreshold_dB, statusCallback);
            break;
        }
        case ChannelMode::AM:
            block = gnuradio::make_block_sptr<ChannelBlockAM>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.squelchThreshold, cc.audioGain_dB,
                cc.dwellTime_s, cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate,
                cc.squelchNoiseMargin_dB, statusCallback);
            break;
        case ChannelMode::NOAA:
            block = gnuradio::make_block_sptr<ChannelBlockEAS>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.squelchThreshold, cc.audioGain_dB,
                cc.dwellTime_s, cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate,
                /*deviation_hz=*/5000, /*alertTonesHz=*/std::vector<double>{1050.0},
                cc.squelchNoiseMargin_dB, statusCallback);
            break;
        case ChannelMode::BFM_EAS:
            block = gnuradio::make_block_sptr<ChannelBlockEAS>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.squelchThreshold, cc.audioGain_dB,
                cc.dwellTime_s, cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate,
                /*deviation_hz=*/75000, /*alertTonesHz=*/std::vector<double>{853.0, 960.0},
                cc.squelchNoiseMargin_dB, statusCallback);
            break;
        case ChannelMode::DMR: {
            if (!cc.dmrSlot.has_value()) {
                throw std::runtime_error("buildChannelBlock: DMR channel missing dmrSlot");
            }
            auto it = dmrDecodersByFreq.find(cc.freq_hz);
            std::shared_ptr<DsdccDecodeBlock> existingDecodeBlock =
                (it != dmrDecodersByFreq.end()) ? it->second : nullptr;
            auto dmrBlock = gnuradio::make_block_sptr<ChannelBlockDMR>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.audioGain_dB, cc.dwellTime_s,
                cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate, *cc.dmrSlot,
                cc.dmrTalkgroupFilter, existingDecodeBlock, statusCallback);
            dmrDecodersByFreq[cc.freq_hz] = dmrBlock->decodeBlock();
            block = dmrBlock;
            break;
        }
        case ChannelMode::P25:
            block = gnuradio::make_block_sptr<ChannelBlockP25Voice>(
                cc.id, cc.label, cc.mute, cc.solo, cc.hold, cc.audioGain_dB, cc.dwellTime_s,
                cc.freq_hz, hardwareFreq_hz, rfSampleRate, audioSampleRate, statusCallback);
            break;
        default:
            throw std::runtime_error("buildChannelBlock: unhandled ChannelMode");
    }
    // forceActive is runtime-only (not a constructor param - see ChannelConfig::forceActive)
    // and defaults false on every fresh block, so it has to be re-applied explicitly here or a
    // rebuild (any structural config change, e.g. re-enabling a channel, adding another one)
    // silently drops a live "Force Active" back to normal squelch gating.
    block->setForceActive(cc.forceActive);
    return block;
}

} // namespace

ScanWindowBlock::ScanWindowBlock(const std::vector<std::shared_ptr<ChannelBlockBase>>& channels,
                                  int audioSampleRate, int globalAudioSampleRate)
    : gr::hier_block2("ScanWindow",
                       gr::io_signature::make(1, 1, sizeof(gr_complex)),
                       gr::io_signature::make(1, 1, sizeof(float))) {

    mixerAdd_ = gr::blocks::add_ff::make(1);

    int channelIdx = 0;
    for (const auto& channel : channels) {
        connect(self(), 0, channel, 0);
        connect(channel, 0, mixerAdd_, channelIdx);
        channelIdx++;
    }

    if (audioSampleRate == globalAudioSampleRate) {
        connect(mixerAdd_, 0, self(), 0);
        return;
    }

    // Need to resample this window's audio to the global rate - find the smallest
    // interpolation/decimation ratio via common-factor reduction (mirrors ScanWindow.py).
    int interp = globalAudioSampleRate;
    int decim = audioSampleRate;
    int n = 2;
    while (n < interp) {
        if (interp % n == 0 && decim % n == 0) {
            interp /= n;
            decim /= n;
        } else {
            n++;
        }
    }

    auto taps = gr::filter::firdes::low_pass(
        1.0, interp, 0.5 * std::min(1.0, static_cast<double>(interp) / decim), 0.05);
    resampler_ = gr::filter::rational_resampler_fff::make(interp, decim, taps);
    connect(mixerAdd_, 0, resampler_, 0);
    connect(resampler_, 0, self(), 0);
}

ScanWindow::ScanWindow(const ScanWindowConfig& config,
                        int rfSampleRate,
                        std::function<void(ChannelStatusUpdate)> statusCallback)
    : id_(config.id.empty() ? makeUuid() : config.id),
      hardwareFreq_hz_(config.hardwareFreq_hz),
      rfSampleRate_(rfSampleRate),
      audioSampleRate_(selectAudioSampleRate(rfSampleRate)) {

    DmrDecodersByFreq dmrDecodersByFreq;
    for (const auto& cc : config.channelConfigs) {
        channels_.push_back(buildChannelBlock(cc, hardwareFreq_hz_, rfSampleRate_, audioSampleRate_,
                                                dmrDecodersByFreq, statusCallback));
    }

    scanWindowBlock_ = gnuradio::make_block_sptr<ScanWindowBlock>(channels_, audioSampleRate_, AUDIO_SAMPLERATE);
}

std::shared_ptr<ChannelBlockBase> ScanWindow::channelById(const std::string& channelId) const {
    for (const auto& c : channels_) {
        if (c->id() == channelId) return c;
    }
    return nullptr;
}

bool ScanWindow::isActive() {
    bool active = false;
    for (auto& c : channels_) {
        if (c->getStatus() != ChannelStatus::IDLE) {
            active = true;
        }
    }
    return active;
}

double ScanWindow::getMinimumScanTime() const {
    if (!minimumScanTime_.has_value()) {
        double maxTime = 0.1;
        for (const auto& c : channels_) {
            maxTime = std::max(maxTime, c->getMinimumScanTime());
        }
        minimumScanTime_ = maxTime;
    }
    return *minimumScanTime_;
}

int ScanWindow::selectRfSampleRate(const std::vector<int>& availableRates, int64_t rfBandwidth) {
    std::vector<int> candidates;
    for (int r : availableRates) {
        if (r >= rfBandwidth) candidates.push_back(r);
    }
    if (candidates.empty()) {
        throw std::runtime_error("ScanWindow: no available RF sample rate covers the required bandwidth");
    }
    return *std::min_element(candidates.begin(), candidates.end());
}

int ScanWindow::selectAudioSampleRate(int rfSampleRate) {
    if (rfSampleRate % AUDIO_SAMPLERATE == 0) {
        return AUDIO_SAMPLERATE;
    }
    for (int n = rfSampleRate / AUDIO_SAMPLERATE; n > 0; n--) {
        if (rfSampleRate % n == 0) {
            return rfSampleRate / n;
        }
    }
    throw std::runtime_error("ScanWindow: could not find a suitable audio sample rate");
}

} // namespace sdrscan
