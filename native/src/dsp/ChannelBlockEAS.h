#pragma once

#include <gnuradio/blocks/mute.h>
#include <gnuradio/blocks/stream_to_vector.h>

#include <vector>

#include "ChannelBlockBase.h"
#include "ChannelBlockFM.h"
#include "HelperBlocks.h"

namespace sdrscan {

// EAS/SAME attention-tone detector, C++ port of Channel.py's ChannelBlock_EAS. Wraps an
// internal ChannelBlockFM (the actual FM demod + RF power squelch) and taps its demodulated
// audio with an FFT tone detector (EasToneDetectBlock, see HelperBlocks.h) looking for the
// configured alert tone(s). Used for both:
//   - NOAA weather radio (deviation_hz=5000, alertTonesHz={1050} - the NOAA/SAME attention
//     tone)
//   - Broadcast EAS (deviation_hz=75000/wideband FM, alertTonesHz={853, 960} - the two-tone
//     EAS attention signal, which relies on ChannelBlockFM's wideband quad-rate support)
// which only differ in those two constructor parameters (see ScanWindow.cpp's
// buildChannelBlock).
//
// Unlike FM/AM, "active" here is a latch rather than a direct squelch reading: 3 consecutive
// tone-detect triggers (debounces false positives from momentary spectral coincidences) opens
// the audio gate and starts a dwellTime_s countdown - the channel stays reported ACTIVE and
// audible until that countdown expires, even if the tone briefly drops out between FFT frames
// (real EAS/SAME tone bursts run several seconds but aren't perfectly continuous frame to
// frame).
class ChannelBlockEAS : public ChannelBlockBase {
public:
    ChannelBlockEAS(const std::string& channelId,
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
                     std::optional<double> squelchNoiseMargin_dB,
                     std::function<void(ChannelStatusUpdate)> statusCallback);

    double getMinimumScanTime() const override { return 0.2; }

    void setForceActive(bool forceActive) override;
    // Not exposed as a knob in the Python original (a documented `# TODO set Volume /
    // Squelch` there) - here they sensibly control the internal FM demod's own squelch/gain,
    // since that's what actually gates/scales the audio a listener hears.
    void setSquelchValue(double squelchThreshold) override;
    void setSquelchNoiseMargin(std::optional<double> marginDb) override;
    void setAudioGain(double audioGain_dB) override;
    ChannelStatus getStatus() override;

private:
    void onToneDetect(bool active);

    std::shared_ptr<ChannelBlockFM> blockFm_;
    gr::blocks::stream_to_vector::sptr blockStreamToVector_;
    std::shared_ptr<EasToneDetectBlock> blockToneDetect_;
    gr::blocks::mute_ff::sptr blockEasAudioMute_;

    int triggerCount_ = 0;
    double timeoutTime_ = 0.0;
};

} // namespace sdrscan
