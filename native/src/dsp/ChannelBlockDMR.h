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

    // Unlike FM/AM's power squelch (which just needs a fraction of a second to sample carrier
    // level), DSDcc's DMR frame sync needs real, *continuous* discriminator samples across
    // several consecutive TDMA bursts (60ms/frame) to lock on at all - the base class's 0.1s
    // default (tuned for analog squelch) is nowhere near enough. Confirmed via a real-hardware
    // capture with the sync-mismatch diagnostic logging on: even a strong signal (repeater
    // antenna ~20 feet from the receiver, ruling out weak-signal theories entirely) showed sync
    // constantly flapping between DMRDataP/DMRVoiceP (correct repeater framing) and
    // DMRDataMS/DMRVoiceMS (DSDcc's fallback when it can't confirm the real pattern) and
    // dropping back to None within the same second - because this receiver's round-robin
    // scheduler (Scanner::buildWindows()/SoapyReceiver::checkCurrentWindow()) only feeds this
    // channel's window real RF for as long as getMinimumScanTime() before it's eligible to hop
    // away again, and every other window's channels get zero samples in between. A DMR
    // transmission spread across scattered ~100ms fragments, seconds apart, can never build a
    // stable lock - DSDcc has no way to know the gap happened, so each fragment starts the sync
    // hunt over from a phase relationship to the transmitter's TDMA clock. This isn't tunable
    // per-channel (dwellTime_s only controls how long an *active* call is held once synced) -
    // it needs a materially longer guaranteed-continuous look before the round-robin considers
    // giving up on this window, same reason real trunked/digital scanners dwell longer on
    // digital channels than analog ones.
    double getMinimumScanTime() const override { return 1.5; }

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
    // Only set when rfSampleRate isn't already a whole multiple of the discriminator rate - see
    // the constructor's rational-resampler comment. Null (skipped) when it divides evenly, same
    // as before this correction existed.
    gr::filter::rational_resampler_ccf::sptr blockRateCorrector_;
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
