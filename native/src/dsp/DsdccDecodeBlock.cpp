#include "DsdccDecodeBlock.h"

#include <gnuradio/io_signature.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace sdrscan {

namespace {
// DSDDecoder::run() takes S16LE discriminator samples (the same convention as classic
// rtl_fm|dsd usage this codec family descends from - see native/README.md). DSDcc's symbol
// timing/level tracking (DSDSymbol) auto-adapts to the observed min/max level rather than
// requiring exact calibration, so this just needs to land in a reasonable, unclipped range -
// unverified against real DMR/P25 RF in this sandbox (no hardware available), flag for
// real-world tuning if decode quality is poor.
constexpr float kInt16Scale = 32767.0f;

// Input (48kHz discriminator, see this class's header) samples per audio-rate output item -
// matches set_relative_rate() below and mbelib's fixed 8kHz decode rate (also duplicated as
// kDiscriminatorRate/kMbeAudioRate in ChannelBlockDMR.cpp/ChannelBlockP25Voice.cpp, which
// resample this block's output the rest of the way to each window's actual audio rate).
constexpr int kInputPerAudioSample = 48000 / 8000;

const char* syncTypeName(DSDcc::DSDDecoder::DSDSyncType t) {
    using T = DSDcc::DSDDecoder::DSDSyncType;
    switch (t) {
        case T::DSDSyncDMRDataP: return "DMRDataP";
        case T::DSDSyncDMRDataMS: return "DMRDataMS";
        case T::DSDSyncDMRVoiceP: return "DMRVoiceP";
        case T::DSDSyncDMRVoiceMS: return "DMRVoiceMS";
        case T::DSDSyncP25p1P: return "P25p1P";
        case T::DSDSyncP25p1N: return "P25p1N";
        case T::DSDSyncNone: return "None";
        default: return "other";
    }
}
} // namespace

DsdccDecodeBlock::DsdccDecodeBlock(DSDcc::DSDDecoder::DSDDecodeMode mode, bool tdmaStereo)
    : gr::block("DsdccDecode",
                gr::io_signature::make(1, 1, sizeof(float)),
                gr::io_signature::make(2, 2, sizeof(float))) {
    decoder_.setQuiet();
    decoder_.enableMbelib(true);
    decoder_.setDecodeMode(mode, true);
    decoder_.setStereo(tdmaStereo);
    // mbelib decodes at 8kHz, far below our 48kHz discriminator input, and arrives in bursts
    // once DSDcc syncs to a frame - just a hint for GNU Radio's buffer sizing, not enforced
    // (general_work() below reports each output port's real count via produce()).
    set_relative_rate(1, 6);
}

int DsdccDecodeBlock::general_work(int noutput_items,
                                    gr_vector_int& ninput_items,
                                    gr_vector_const_void_star& input_items,
                                    gr_vector_void_star& output_items) {
    const float* in = static_cast<const float*>(input_items[0]);
    int nin = ninput_items[0];

    // Must advance at a fixed rate (kInputPerAudioSample input samples per output item) rather
    // than only whenever DSDcc happens to have decoded something: this feeds
    // ScanWindowBlock::mixerAdd_ (a synchronous gr::blocks::add_ff), which can't produce ANY
    // output until every one of its connected ports has data. A channel that goes fully silent
    // (produces zero items) for as long as it isn't mid-voice-frame - which in production is
    // most of the time - silently stalls the *entire* window's audio, not just its own, and the
    // backpressure eventually freezes every other channel's RSSI/volume computation too. Real
    // decoded audio (pending1_/pending2_) is used when available; gaps are zero-filled, exactly
    // like every other channel mode's audio gate (mute_ff) already does when squelched.
    int itemsToProduce = std::min(nin / kInputPerAudioSample, noutput_items);
    int consumed = itemsToProduce * kInputPerAudioSample;

    for (int i = 0; i < consumed; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, in[i]));
        decoder_.run(static_cast<short>(std::lround(clamped * kInt16Scale)));
    }
    consume_each(consumed);

    logSyncTypeChange();
    pollDecodedAudio();

    float* out1 = static_cast<float*>(output_items[0]);
    float* out2 = static_cast<float*>(output_items[1]);
    int n1 = std::min(static_cast<int>(pending1_.size()), itemsToProduce);
    int n2 = std::min(static_cast<int>(pending2_.size()), itemsToProduce);
    std::copy(pending1_.begin(), pending1_.begin() + n1, out1);
    std::copy(pending2_.begin(), pending2_.begin() + n2, out2);
    pending1_.erase(pending1_.begin(), pending1_.begin() + n1);
    pending2_.erase(pending2_.begin(), pending2_.begin() + n2);
    std::fill(out1 + n1, out1 + itemsToProduce, 0.0f);
    std::fill(out2 + n2, out2 + itemsToProduce, 0.0f);

    produce(0, itemsToProduce);
    produce(1, itemsToProduce);
    return WORK_CALLED_PRODUCE;
}

void DsdccDecodeBlock::logSyncTypeChange() {
    auto syncType = decoder_.getSyncType();
    if (syncType == lastLoggedSyncType_) return;
    // DMR-specific diagnostic (see DSDDecoder::getDmrDataBsSyncErrors() etc., a small vendored
    // patch to DSDcc - native/dsdcc-diagnostics.patch): the sync engine's mismatch count for
    // each DMR pattern (0-24 dibits, tolerance 2) from the most recent sync search, regardless
    // of which pattern actually won this transition. Answers "how close was BS-framed (real
    // repeater) sync to matching" whenever MS-framed (direct-mode) sync wins instead, which
    // otherwise looks identical to genuinely MS-only traffic in the plain sync-type log alone.
    std::cerr << "DsdccDecodeBlock: sync " << syncTypeName(lastLoggedSyncType_) << " -> "
               << syncTypeName(syncType) << " (DMR sync errors: dataBS=" << decoder_.getDmrDataBsSyncErrors()
               << " dataMS=" << decoder_.getDmrDataMsSyncErrors()
               << " voiceBS=" << decoder_.getDmrVoiceBsSyncErrors()
               << " voiceMS=" << decoder_.getDmrVoiceMsSyncErrors() << ", tolerance=2)\n";
    lastLoggedSyncType_ = syncType;
}

void DsdccDecodeBlock::pollDecodedAudio() {
    int n1 = 0;
    short* a1 = decoder_.getAudio1(n1);
    for (int i = 0; i < n1; i++) pending1_.push_back(a1[i] / kInt16Scale);
    if (n1 > 0) decoder_.resetAudio1();

    int n2 = 0;
    short* a2 = decoder_.getAudio2(n2);
    for (int i = 0; i < n2; i++) pending2_.push_back(a2[i] / kInt16Scale);
    if (n2 > 0) decoder_.resetAudio2();
}

} // namespace sdrscan
