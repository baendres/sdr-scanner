#pragma once

#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/filter/rational_resampler.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/mute.h>

#include "ChannelBlockBase.h"
#include "DsdccDecodeBlock.h"

namespace sdrscan {

// Conventional (non-trunked) DMR demodulator - one timeslot per instance (see ChannelConfig::
// dmrSlot). Channelizes the shared window RF input down to a 48kHz discriminator stream (C4FM/
// 4FSK, same quadrature-demod technique as ChannelBlockFM, just no de-emphasis/CTCSS - those
// are FM broadcast-specific) and feeds it into a DsdccDecodeBlock, which does DMR frame sync/
// deframe/AMBE decode via DSDcc+mbelib (see native/README.md).
//
// TS1/TS2 share one underlying C4FM front end + DsdccDecodeBlock rather than each independently
// demodulating the same RF (DSDcc already decodes both TDMA slots from one input stream - see
// DsdccDecodeBlock's header). Whichever ChannelBlockDMR is constructed first for a given
// frequency (see ScanWindow::buildChannelBlock's freq_hz-keyed lookup) builds the real front
// end/decoder and is passed to the second one via existingDecodeBlock - the second instance
// discards its own copy of the window's RF stream into a null_sink (GNU Radio requires every
// hier_block2 boundary port connected internally - see the analogous null_sink usage in
// ChannelBlockFM's CTCSS side-tap) and taps the shared decoder's other slot output instead.
//
// No power squelch - DMR's "squelch" is DSDcc's own frame-sync/voice-activity detection
// (getVoice1On()/getVoice2On()), polled in getStatus(). setSquelchValue() is a no-op (there's
// no adjustable threshold); the base class still requires overriding it.
class ChannelBlockDMR : public ChannelBlockBase {
public:
    ChannelBlockDMR(const std::string& channelId,
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
                     int dmrSlot,
                     std::optional<uint32_t> talkgroupFilter,
                     std::shared_ptr<DsdccDecodeBlock> existingDecodeBlock,
                     bool otherSlotPresent,
                     std::function<void(ChannelStatusUpdate)> statusCallback);

    void setForceActive(bool forceActive) override;
    void setSquelchValue(double squelchThreshold) override;
    void setAudioGain(double audioGain_dB) override;
    void setDmrTalkgroupFilter(std::optional<uint32_t> talkgroupFilter) override { talkgroupFilter_ = talkgroupFilter; }
    ChannelStatus getStatus() override;

    // Non-null once constructed (either freshly built, for whichever instance owns the front
    // end, or the one passed into existingDecodeBlock) - lets ScanWindow::buildChannelBlock pass
    // the first DMR channel's decoder to the second one at the same freq_hz.
    std::shared_ptr<DsdccDecodeBlock> decodeBlock() const { return decodeBlock_; }

private:
    // Parses the talkgroup number DSDcc formats into DSDDMR::getSlot0Text()/getSlot1Text() at
    // fixed offset 18, 8 digits (see native/README.md's DSDcc caveats - not exposed as a
    // queryable numeric field upstream). Returns nullopt if the text isn't in the expected
    // format yet (no call decoded on this slot since sync).
    std::optional<uint32_t> currentTalkgroup() const;

    int dmrSlot_; // 1 or 2
    std::optional<uint32_t> talkgroupFilter_;
    std::shared_ptr<DsdccDecodeBlock> decodeBlock_;

    // Only set when this instance owns the front end (existingDecodeBlock was null) - the
    // shared-tap instance leaves these null and just wires decodeBlock_'s port directly.
    gr::filter::freq_xlating_fir_filter_ccf::sptr blockFreqXlatingFilter_;
    gr::filter::fir_filter_ccf::sptr blockChannelFilter_;
    gr::analog::quadrature_demod_cf::sptr blockQuadDemod_;
    gr::blocks::null_sink::sptr blockRfDiscardSink_; // shared-tap instance only
    // Owning instance only, and only when otherSlotPresent is false (see constructor) - DsdccDecodeBlock
    // requires both its output ports connected regardless of whether a real channel exists for
    // the other slot in this window, so this discards it the same way blockRfDiscardSink_
    // discards an unused RF input.
    gr::blocks::null_sink::sptr blockUnusedSlotSink_;

    gr::filter::rational_resampler_fff::sptr blockResampler_; // decoded 8kHz -> audioSampleRate_
    gr::blocks::multiply_const_ff::sptr blockAudioGain_;
    // Voice-activity gate, driven from getStatus() - separate from the base class's
    // blockAudioMute_ (driven only by user mute/solo, see ChannelBlockBase::setMute()), same
    // two-gate pattern ChannelBlockFM uses for CTCSS/squelch vs. user mute.
    gr::blocks::mute_ff::sptr blockAudioGate_;
};

} // namespace sdrscan
