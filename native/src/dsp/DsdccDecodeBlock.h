#pragma once

#include <gnuradio/block.h>

#include <dsdcc/dsd_decoder.h>

#include <vector>

namespace sdrscan {

// Wraps a DSDcc::DSDDecoder as a GNU Radio block: consumes float discriminator-audio samples
// at 48kHz (matching DSDcc's DSDRate4800 - 10 samples/symbol at 4800 baud, the rate
// DSDDecoder::setDecodeMode() selects for both DMR and P25 Phase 1 - see
// native/README.md's DSDcc integration notes) on its single input, and produces two
// independently-timed decoded-audio streams on its two outputs: DMR timeslot 1 / timeslot 2
// (output 0 / output 1), or the single P25 Phase 1 voice stream on output 0 only (output 1
// stays silent - DSDcc has no second "slot" concept for P25p1). Decoded audio arrives from
// mbelib in bursts, not at a fixed rate relative to the 48kHz input (DSDcc has to first sync to
// a frame), so this is a gr::block (arbitrary rate) rather than gr::sync_block.
//
// Talkgroup ID / voice-activity state (getVoice1On()/getVoice2On(), and the talkgroup number
// parsed out of DSDDMR::getSlot0Text()/getSlot1Text() - see native/README.md's DSDcc caveats)
// is polled directly off decoder() by ChannelBlockDMR, not surfaced through the stream ports.
class DsdccDecodeBlock : public gr::block {
public:
    DsdccDecodeBlock(DSDcc::DSDDecoder::DSDDecodeMode mode, bool tdmaStereo);

    int general_work(int noutput_items,
                      gr_vector_int& ninput_items,
                      gr_vector_const_void_star& input_items,
                      gr_vector_void_star& output_items) override;

    DSDcc::DSDDecoder& decoder() { return decoder_; }

private:
    void pollDecodedAudio();
    void logSyncTypeChange();

    DSDcc::DSDDecoder decoder_;
    std::vector<float> pending1_;
    std::vector<float> pending2_;
    // Diagnostic aid for tuning against real RF (see this class's header caveat) - logs to
    // stderr whenever DSDcc's sync state changes, so it's possible to tell "never syncing at
    // all" (an RF-chain/calibration problem) from "syncs but voice-active never triggers" (a
    // logic bug further downstream in ChannelBlockDMR/ChannelBlockP25Voice) just by watching the
    // server console while transmitting on the channel's frequency.
    DSDcc::DSDDecoder::DSDSyncType lastLoggedSyncType_ = DSDcc::DSDDecoder::DSDSyncNone;
};

} // namespace sdrscan
