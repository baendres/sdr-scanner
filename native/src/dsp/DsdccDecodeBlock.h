#pragma once

#include <gnuradio/block.h>

#include <dsdcc/dsd_decoder.h>

#include <string>
#include <vector>

namespace sdrscan {

// Wraps a DSDcc::DSDDecoder as a GNU Radio block: consumes float discriminator-audio samples
// at 48kHz (matching DSDcc's DSDRate4800 - 10 samples/symbol at 4800 baud, the rate
// DSDDecoder::setDecodeMode() selects for both DMR and P25 Phase 1 - see
// native/README.md's DSDcc integration notes) on its single input, and produces two
// two decoded-audio streams on its two outputs: DMR timeslot 1 / timeslot 2 (output 0 / output
// 1), or the single P25 Phase 1 voice stream on output 0 only (output 1 stays silent - DSDcc
// has no second "slot" concept for P25p1). mbelib only actually decodes audio in bursts (DSDcc
// has to first sync to a frame), but general_work() always produces output at a fixed rate
// (kInputPerAudioSample input samples per output item, matching set_relative_rate() below),
// zero-filling whenever nothing's been decoded yet - so this is a gr::block (needed for the
// input:output ratio, and to poll DSDcc's decoder state each call) rather than gr::sync_block,
// but its *output* behaves like any synchronous audio stream (continuous, real audio or
// silence, never absent). This matters: it feeds ScanWindowBlock::mixerAdd_, a synchronous
// gr::blocks::add_ff that can't produce any output until every connected port has data - a
// channel that ever stopped producing entirely (as an earlier version of this block did
// whenever DSDcc had nothing newly decoded, which in production is most of the time) would
// silently stall the whole window's audio, not just its own.
//
// Talkgroup ID / voice-activity state (getVoice1On()/getVoice2On(), and the talkgroup number
// parsed out of DSDDMR::getSlot0Text()/getSlot1Text() - see native/README.md's DSDcc caveats)
// is polled directly off decoder() by ChannelBlockDMR, not surfaced through the stream ports.
class DsdccDecodeBlock : public gr::block {
public:
    // logLabel is diagnostic-only (prefixes logSyncTypeChange()'s stderr output, e.g. a
    // channel's freq_hz) - lets a multi-channel/multi-receiver log be attributed to the right
    // frequency instead of every DsdccDecodeBlock instance logging identically.
    DsdccDecodeBlock(DSDcc::DSDDecoder::DSDDecodeMode mode, bool tdmaStereo,
                      std::string logLabel = "");

    int general_work(int noutput_items,
                      gr_vector_int& ninput_items,
                      gr_vector_const_void_star& input_items,
                      gr_vector_void_star& output_items) override;

    DSDcc::DSDDecoder& decoder() { return decoder_; }

private:
    void pollDecodedAudio(int itemsThisCall);
    void logSyncTypeChange();

    DSDcc::DSDDecoder decoder_;
    std::string logLabel_;
    std::vector<float> pending1_;
    std::vector<float> pending2_;
    // Diagnostic aid for tuning against real RF (see this class's header caveat) - logs to
    // stderr whenever DSDcc's sync state changes, so it's possible to tell "never syncing at
    // all" (an RF-chain/calibration problem) from "syncs but voice-active never triggers" (a
    // logic bug further downstream in ChannelBlockDMR/ChannelBlockP25Voice) just by watching the
    // server console while transmitting on the channel's frequency.
    DSDcc::DSDDecoder::DSDSyncType lastLoggedSyncType_ = DSDcc::DSDDecoder::DSDSyncNone;
    // When lastLoggedSyncType_ was last set - lets logSyncTypeChange() report how long the
    // previous sync type actually held, which is what tells "genuinely never held a lock" (a
    // real decode problem) apart from "held fine, just isn't the pattern expected" from the
    // plain transition log alone.
    double lastTransitionAt_ = 0.0;
};

} // namespace sdrscan
