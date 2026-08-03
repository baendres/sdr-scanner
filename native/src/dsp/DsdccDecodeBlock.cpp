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

    for (int i = 0; i < nin; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, in[i]));
        decoder_.run(static_cast<short>(std::lround(clamped * kInt16Scale)));
    }
    consume_each(nin);

    logSyncTypeChange();
    pollDecodedAudio();

    float* out1 = static_cast<float*>(output_items[0]);
    float* out2 = static_cast<float*>(output_items[1]);
    int n1 = std::min(static_cast<int>(pending1_.size()), noutput_items);
    int n2 = std::min(static_cast<int>(pending2_.size()), noutput_items);
    std::copy(pending1_.begin(), pending1_.begin() + n1, out1);
    std::copy(pending2_.begin(), pending2_.begin() + n2, out2);
    pending1_.erase(pending1_.begin(), pending1_.begin() + n1);
    pending2_.erase(pending2_.begin(), pending2_.begin() + n2);

    produce(0, n1);
    produce(1, n2);
    return WORK_CALLED_PRODUCE;
}

void DsdccDecodeBlock::logSyncTypeChange() {
    auto syncType = decoder_.getSyncType();
    if (syncType == lastLoggedSyncType_) return;
    std::cerr << "DsdccDecodeBlock: sync " << syncTypeName(lastLoggedSyncType_) << " -> "
               << syncTypeName(syncType) << "\n";
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
