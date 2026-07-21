#include "ChannelBlockEAS.h"

#include <gnuradio/sptr_magic.h>

#include "../util/Time.h"

namespace sdrscan {

namespace {
constexpr int kFftSize = 1024;
constexpr double kToneThresholdDb = 20.0;
constexpr double kRefBandLowHz = 1100.0;
constexpr double kRefBandHighHz = 1200.0;
constexpr int kTriggersToActivate = 3; // debounces momentary spectral false positives
} // namespace

ChannelBlockEAS::ChannelBlockEAS(const std::string& channelId,
                                  const std::string& label,
                                  bool mute,
                                  TriBool solo,
                                  bool hold,
                                  double squelchThreshold,
                                  double audioGain_dB,
                                  double dwellTime_s,
                                  int64_t channelFreq_hz,
                                  int64_t hardwareFreq_hz,
                                  int rfSampleRate,
                                  int audioSampleRate,
                                  int deviation_hz,
                                  std::vector<double> alertTonesHz,
                                  std::function<void(ChannelStatusUpdate)> statusCallback)
    : ChannelBlockBase(channelId, label, mute, solo, hold, squelchThreshold, audioGain_dB,
                        dwellTime_s, audioSampleRate, std::move(statusCallback)) {

    // The internal FM block's own mute/solo/hold/status-callback are never driven or consulted
    // (this class owns those concerns at the EAS level - see setForceActive()/getStatus()); it
    // exists purely to demodulate audio and provide an RF power squelch gate ahead of the tone
    // detector.
    blockFm_ = gnuradio::make_block_sptr<ChannelBlockFM>(
        channelId, label, /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        squelchThreshold, audioGain_dB, dwellTime_s, channelFreq_hz, hardwareFreq_hz,
        rfSampleRate, audioSampleRate, deviation_hz, /*ctcssToneHz=*/std::nullopt,
        [](ChannelStatusUpdate) {});

    blockStreamToVector_ = gr::blocks::stream_to_vector::make(sizeof(float), kFftSize);
    blockToneDetect_ = gnuradio::make_block_sptr<EasToneDetectBlock>(
        [this](bool active) { onToneDetect(active); },
        alertTonesHz, kRefBandLowHz, kRefBandHighHz, kToneThresholdDb, kFftSize, audioSampleRate_);

    blockEasAudioMute_ = gr::blocks::mute_ff::make(true);

    // Real audio path: self -> FM demod -> EAS trigger gate -> (base class) mute/solo gate ->
    // self. blockAudioMute_ and its wiring to self()'s output are set up by ChannelBlockBase's
    // constructor already.
    connect(self(), 0, blockFm_, 0);
    connect(blockFm_, 0, blockEasAudioMute_, 0);
    connect(blockEasAudioMute_, 0, blockAudioMute_, 0);

    // Side tap: FM demod output -> tone detector (detection only, not in the audio path).
    connect(blockFm_, 0, blockStreamToVector_, 0);
    connect(blockStreamToVector_, 0, blockToneDetect_, 0);
}

void ChannelBlockEAS::onToneDetect(bool active) {
    if (active) {
        triggerCount_++;
        if (triggerCount_ >= kTriggersToActivate) {
            blockEasAudioMute_->set_mute(false);
            active_ = true;
            lastActive_ = nowUnixSeconds();
            timeoutTime_ = lastActive_ + dwellTime_s_;
        }
    } else {
        triggerCount_ = 0;
    }
}

void ChannelBlockEAS::setForceActive(bool forceActive) {
    forceActive_ = forceActive;
    if (forceActive) {
        blockFm_->setForceActive(true);
        blockEasAudioMute_->set_mute(false);
        active_ = true;
    } else {
        blockFm_->setForceActive(false);
        timeoutTime_ = 0.0;
    }
}

void ChannelBlockEAS::setSquelchValue(double squelchThreshold) {
    squelchThreshold_ = squelchThreshold;
    blockFm_->setSquelchValue(squelchThreshold);
}

void ChannelBlockEAS::setAudioGain(double audioGain_dB) {
    audioGainFactor_ = dbToRatio(audioGain_dB);
    blockFm_->setAudioGain(audioGain_dB);
}

ChannelStatus ChannelBlockEAS::getStatus() {
    ChannelStatus status = hold_ ? ChannelStatus::HOLD : ChannelStatus::IDLE;

    if (active_ || forceActive_) {
        active_ = true;
        if (forceActive_) {
            status = ChannelStatus::FORCE_ACTIVE;
        } else {
            status = ChannelStatus::ACTIVE;
            if (nowUnixSeconds() > timeoutTime_) {
                active_ = false;
                blockEasAudioMute_->set_mute(true);
            }
        }
    } else if (triggerCount_ > 0) {
        // In a pre-trigger state - keep the ScanWindow from being considered idle while we're
        // partway through debouncing a possible tone.
        status = ChannelStatus::DWELL;
    }

    // Telemetry comes from the internal FM demod, matching Channel.py reading
    // self.blockFM._rssi etc directly - see the accessor comments on ChannelBlockBase.
    rssi_dBFS_ = blockFm_->rssi();
    noiseFloor_dBFS_ = blockFm_->noiseFloor();
    volume_dBFS_ = blockFm_->volume();

    return reportStatus(status);
}

} // namespace sdrscan
