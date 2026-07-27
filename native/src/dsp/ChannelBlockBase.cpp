#include "ChannelBlockBase.h"
#include "Const.h"

#include <gnuradio/io_signature.h>

#include "../util/Time.h"

namespace sdrscan {

ChannelBlockBase::ChannelBlockBase(const std::string& channelId,
                                    const std::string& label,
                                    bool mute,
                                    TriBool solo,
                                    bool hold,
                                    double squelchThreshold,
                                    double audioGain_dB,
                                    double dwellTime_s,
                                    int audioSampleRate,
                                    std::optional<double> squelchNoiseMargin_dB,
                                    std::function<void(ChannelStatusUpdate)> statusCallback)
    : gr::hier_block2("Channel",
                       gr::io_signature::make(1, 1, sizeof(gr_complex)),
                       gr::io_signature::make(1, 1, sizeof(float))),
      channelId_(channelId),
      label_(label),
      mute_(mute),
      solo_(solo),
      hold_(hold),
      squelchThreshold_(squelchThreshold),
      squelchNoiseMargin_dB_(squelchNoiseMargin_dB),
      audioGainFactor_(dbToRatio(audioGain_dB)),
      dwellTime_s_(dwellTime_s),
      audioSampleRate_(audioSampleRate),
      statusCallback_(std::move(statusCallback)) {

    blockAudioMute_ = gr::blocks::mute_ff::make(false);
    connect(blockAudioMute_, 0, self(), 0);

    double attackAlpha = 1.0 / (audioSampleRate_ * VOLUME_LOWPASS_ATTACK_TC);
    double decayAlpha = 1.0 / (audioSampleRate_ * VOLUME_LOWPASS_DECAY_TC);
    blockVolume_ = std::make_shared<MagToPowerLowPassBlock>(
        [this](float dBFS) { updateVolume(dBFS); }, attackAlpha, decayAlpha);
}

void ChannelBlockBase::connectVolume(const gr::basic_block_sptr& sourceBlock, int sourceBlockPort) {
    connect(sourceBlock, sourceBlockPort, blockVolume_, 0);
}

void ChannelBlockBase::updateRSSI(float dBFS) {
    rssi_dBFS_ = dBFS;
    if (!active_) {
        if (!noiseFloor_dBFS_.has_value()) {
            noiseFloor_dBFS_ = dBFS;
        } else {
            noiseFloor_dBFS_ = (NOISEFLOOR_LOWPASS_A * dBFS) + ((1 - NOISEFLOOR_LOWPASS_A) * (*noiseFloor_dBFS_));
        }
        // Deliberately doesn't push the new threshold to blockPowerSquelch_ here - this runs on
        // the flowgraph's own worker thread (called from Mag2ToPowerBlock's work()), and every
        // other hot-update setter in this codebase is only ever called from Scanner's separate
        // control-plane thread. getStatus() (already on that safe thread, polled every ~100ms)
        // applies the updated threshold instead - see ChannelBlockFM/AM::getStatus().
    }
}

double ChannelBlockBase::effectiveSquelchThreshold() const {
    if (squelchNoiseMargin_dB_.has_value() && noiseFloor_dBFS_.has_value()) {
        return static_cast<double>(*noiseFloor_dBFS_) + *squelchNoiseMargin_dB_;
    }
    return squelchThreshold_;
}

bool ChannelBlockBase::adaptiveThresholdChanged(double newThreshold) {
    if (lastPushedAdaptiveThreshold_.has_value() && *lastPushedAdaptiveThreshold_ == newThreshold) {
        return false;
    }
    lastPushedAdaptiveThreshold_ = newThreshold;
    return true;
}

bool ChannelBlockBase::setAudioGateMuted(const gr::blocks::mute_ff::sptr& gate, bool muted) {
    if (lastAudioGateMuted_.has_value() && *lastAudioGateMuted_ == muted) {
        return false;
    }
    lastAudioGateMuted_ = muted;
    gate->set_mute(muted);
    return true;
}

bool ChannelBlockBase::debounceSquelch(bool rawUnmuted) {
    double now = nowUnixSeconds();
    if (rawUnmuted != debounceRawUnmuted_) {
        debounceRawUnmuted_ = rawUnmuted;
        debounceRawChangedAt_ = now;
    }
    if (rawUnmuted != debounceStableUnmuted_ && (now - debounceRawChangedAt_) >= SQUELCH_DEBOUNCE_SECONDS) {
        debounceStableUnmuted_ = rawUnmuted;
    }
    return debounceStableUnmuted_;
}

void ChannelBlockBase::updateVolume(float dBFS) {
    volume_dBFS_ = dBFS;
}

void ChannelBlockBase::setMute(bool mute) {
    mute_ = mute;
    bool finalMute = mute_;
    if (solo_.has_value() && !(*solo_)) {
        finalMute = true;
    }
    blockAudioMute_->set_mute(finalMute);
}

void ChannelBlockBase::setSolo(TriBool solo) {
    solo_ = solo;
    setMute(mute_);
}

void ChannelBlockBase::setHold(bool hold) {
    hold_ = hold;
}

ChannelStatus ChannelBlockBase::computeAndReportStatus(bool unmutedNow) {
    ChannelStatus status = hold_ ? ChannelStatus::HOLD : ChannelStatus::IDLE;

    double now = nowUnixSeconds();

    if (unmutedNow) {
        active_ = true;
        lastActive_ = now;
        status = forceActive_ ? ChannelStatus::FORCE_ACTIVE : ChannelStatus::ACTIVE;
    } else {
        active_ = false;
        if (now - lastActive_ < dwellTime_s_) {
            status = ChannelStatus::DWELL;
        }
    }

    return reportStatus(status);
}

ChannelStatus ChannelBlockBase::reportStatus(ChannelStatus status) {
    double now = nowUnixSeconds();

    bool statusChanged = (!lastStatusReport_.has_value() || *lastStatusReport_ != status);
    bool periodicUpdate = status != ChannelStatus::IDLE && (now - lastStatusTime_) > STATUS_UPDATE_TIME_S;

    if (statusChanged || periodicUpdate) {
        lastStatusTime_ = now;
        lastStatusReport_ = status;
        if (statusCallback_) {
            ChannelStatusUpdate update;
            update.channelId = channelId_;
            update.status = status;
            update.rssi_dBFS = rssi_dBFS_;
            update.noiseFloor_dBFS = noiseFloor_dBFS_;
            update.volume_dBFS = volume_dBFS_;
            update.noiseRefLevel_dBFS = noiseRefLevel_dBFS_;
            statusCallback_(update);
        }
    }

    return status;
}

} // namespace sdrscan
