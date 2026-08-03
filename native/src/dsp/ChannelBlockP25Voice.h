#pragma once

#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/filter/rational_resampler.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/mute.h>
#include <gnuradio/blocks/null_sink.h>

#include "ChannelBlockBase.h"
#include "DsdccDecodeBlock.h"

namespace sdrscan {

// P25 Phase 1 conventional (fixed-frequency, non-trunked) voice demodulator. Same C4FM front
// end + DsdccDecodeBlock plumbing as ChannelBlockDMR (see that class's header), just
// DSDDecodeP25P1 mode instead of DSDDecodeDMR - P25 Phase 1 has no timeslot split, so this
// always builds its own front end (no shared-decoder pattern needed) and reads only
// DsdccDecodeBlock's output port 0 (P25 audio always lands there - see DsdccDecodeBlock's
// header; port 1 stays silent for this mode).
//
// No power squelch - same reasoning as ChannelBlockDMR: "squelch" is DSDcc's own frame-sync/
// voice-activity detection (getVoice1On()), polled in getStatus().
class ChannelBlockP25Voice : public ChannelBlockBase {
public:
    ChannelBlockP25Voice(const std::string& channelId,
                          const std::string& label,
                          bool mute,
                          TriBool solo,
                          bool hold,
                          double audioGain_dB,
                          double dwellTime_s,
                          int64_t channelFreq_hz,
                          int64_t hardwareFreq_hz,
                          int rfSampleRate,
                          int audioSampleRate,
                          std::function<void(ChannelStatusUpdate)> statusCallback);

    void setForceActive(bool forceActive) override;
    void setSquelchValue(double squelchThreshold) override;
    void setAudioGain(double audioGain_dB) override;
    ChannelStatus getStatus() override;

    std::shared_ptr<DsdccDecodeBlock> decodeBlock() const { return decodeBlock_; }

private:
    std::shared_ptr<DsdccDecodeBlock> decodeBlock_;

    gr::filter::freq_xlating_fir_filter_ccf::sptr blockFreqXlatingFilter_;
    gr::filter::fir_filter_ccf::sptr blockChannelFilter_;
    gr::analog::quadrature_demod_cf::sptr blockQuadDemod_;

    gr::filter::rational_resampler_fff::sptr blockResampler_; // decoded 8kHz -> audioSampleRate_
    gr::blocks::multiply_const_ff::sptr blockAudioGain_;
    gr::blocks::mute_ff::sptr blockAudioGate_; // voice-activity gate, see ChannelBlockDMR's header
    // DsdccDecodeBlock always exposes 2 output ports (DMR's two timeslots); P25 Phase 1 only
    // ever uses port 0 (see DsdccDecodeBlock's header) - port 1 must still be connected to
    // satisfy GNU Radio's "every declared port wired" requirement, same reasoning as
    // ChannelBlockDMR's shared-tap RF discard.
    gr::blocks::null_sink::sptr blockUnusedSlot2Sink_;
};

} // namespace sdrscan
