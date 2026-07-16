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
    }
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
            statusCallback_(update);
        }
    }

    return status;
}

} // namespace sdrscan
