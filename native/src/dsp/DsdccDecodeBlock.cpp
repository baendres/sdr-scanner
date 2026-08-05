#include "DsdccDecodeBlock.h"
#include "../util/Time.h"

#include <gnuradio/io_signature.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>

namespace sdrscan {

namespace {

// Disambiguates log lines when two DsdccDecodeBlock instances are labeled with the same
// frequency - e.g. the same DMR/P25 channel configured into more than one receiver's scan
// windows, which builds one independent decoder instance per window. Without this, their
// sync-transition logs interleave under an identical "[freq]" tag and read as one impossible,
// self-contradictory state machine (a transition FROM a state that was never logged as entered).
std::atomic<int> g_nextInstanceId{0};
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

DsdccDecodeBlock::DsdccDecodeBlock(DSDcc::DSDDecoder::DSDDecodeMode mode, bool tdmaStereo,
                                    std::string logLabel)
    : gr::block("DsdccDecode",
                gr::io_signature::make(1, 1, sizeof(float)),
                gr::io_signature::make(2, 2, sizeof(float))),
      logLabel_(std::move(logLabel)) {
    int instanceId = g_nextInstanceId.fetch_add(1);
    if (!logLabel_.empty()) {
        logLabel_ += "#" + std::to_string(instanceId);
    }
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

    // Must always consume ALL available input at the real 48kHz rate, regardless of how much
    // *output* buffer space (noutput_items) happens to be available this call - this must never
    // be the thing that throttles input consumption. DSDcc's frame sync assumes a truly
    // continuous, real-time discriminator stream; falling behind here - even briefly, even
    // though GNU Radio's own buffering would normally absorb it losslessly - risks GNU Radio
    // applying backpressure all the way back to the real-time RF source, which can genuinely
    // drop hardware samples and break sync exactly like the window-hopping discontinuity bug
    // fixed elsewhere in this codebase (Scanner/SoapyReceiver's round-robin). An earlier version
    // of this fix capped input consumption at noutput_items*kInputPerAudioSample, which
    // reintroduced exactly that risk - confirmed on real hardware as sync achieving a clean lock
    // (low DMR mismatch count) then losing it well under a second later, during a known-active,
    // continuous real transmission where nothing should have interrupted it.
    //
    // So: consume everything available now, and decouple that from how much gets *written out*
    // this call - pollDecodedAudio() below tops up pending1_/pending2_ (real decoded audio, or
    // zero-fill once there's nothing new - same fixed-rate-output reasoning as before) by
    // however many audio-rate items this call's input earns, regardless of noutput_items; only
    // draining the output arrays is bounded by noutput_items, with any backlog simply waiting in
    // the queue for a later call instead of ever throttling input.
    int itemsThisCall = nin / kInputPerAudioSample;
    int consumed = itemsThisCall * kInputPerAudioSample;

    for (int i = 0; i < consumed; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, in[i]));
        short sample = static_cast<short>(std::lround(clamped * kInt16Scale));
        // DSDDecoder::run() (upstream, dsd_decoder.cpp) treats a literal 0 sample as "external
        // squelch just closed" - after DSD_SQUELCH_TIMEOUT_SAMPLES (960, 20ms at 48kHz)
        // consecutive exact zeros it discards whatever sync it has and goes back to hunting.
        // That's the right behavior for DSDcc's classic calling convention (an external analog
        // squelch gate feeding literal zeros during real silence, e.g. `rtl_fm | dsd`), but this
        // class feeds it raw, unsquelched discriminator output - a real, actively-transmitting
        // signal can still legitimately produce a sample that quantizes to exactly 0 (a fade, a
        // TDMA idle-slot gap, low-amplitude noise between symbols), which DSDcc then
        // misinterprets as silence and throws away a perfectly good lock. Confirmed on real
        // hardware: sync achieved cleanly during a continuous, known 10+ second transmission,
        // then discarded under a second in - nudging a would-be-zero sample to the smallest
        // representable nonzero value (amplitude-wise negligible, ~1/32767 of full scale) keeps
        // DSDcc's own squelch-timeout logic from ever firing on real signal.
        if (sample == 0) sample = 1;
        decoder_.run(sample);
    }
    consume_each(consumed);

    logSyncTypeChange();
    pollDecodedAudio(itemsThisCall);

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
    double now = nowUnixSeconds();
    double heldSeconds = (lastTransitionAt_ > 0.0) ? (now - lastTransitionAt_) : 0.0;
    // DMR-specific diagnostic (see DSDDecoder::getDmrDataBsSyncErrors() etc., a small vendored
    // patch to DSDcc - native/dsdcc-diagnostics.patch): the sync engine's mismatch count for
    // each DMR pattern (0-24 dibits, tolerance 2) from the most recent sync search, regardless
    // of which pattern actually won this transition. Answers "how close was BS-framed (real
    // repeater) sync to matching" whenever MS-framed (direct-mode) sync wins instead, which
    // otherwise looks identical to genuinely MS-only traffic in the plain sync-type log alone.
    // heldSeconds (how long the *previous* state lasted) and logLabel_ (which channel/frequency)
    // together answer "did this ever get a fair continuous shot" directly, instead of having to
    // infer it from a separate, unlabeled SoapyReceiver hop-count log line.
    std::cerr << "DsdccDecodeBlock" << (logLabel_.empty() ? "" : " [" + logLabel_ + "]") << ": sync "
               << syncTypeName(lastLoggedSyncType_) << " -> " << syncTypeName(syncType)
               << " (held " << heldSeconds << "s) (DMR sync errors: dataBS=" << decoder_.getDmrDataBsSyncErrors()
               << " dataMS=" << decoder_.getDmrDataMsSyncErrors()
               << " voiceBS=" << decoder_.getDmrVoiceBsSyncErrors()
               << " voiceMS=" << decoder_.getDmrVoiceMsSyncErrors() << ", tolerance=2)\n";
    lastLoggedSyncType_ = syncType;
    lastTransitionAt_ = now;
}

void DsdccDecodeBlock::pollDecodedAudio(int itemsThisCall) {
    size_t before1 = pending1_.size();
    size_t before2 = pending2_.size();

    int n1 = 0;
    short* a1 = decoder_.getAudio1(n1);
    for (int i = 0; i < n1; i++) pending1_.push_back(a1[i] / kInt16Scale);
    if (n1 > 0) decoder_.resetAudio1();

    int n2 = 0;
    short* a2 = decoder_.getAudio2(n2);
    for (int i = 0; i < n2; i++) pending2_.push_back(a2[i] / kInt16Scale);
    if (n2 > 0) decoder_.resetAudio2();

    // Top up with silence so each queue grows by exactly itemsThisCall this call (matching how
    // many audio-rate items this call's real-time input earns - see general_work()'s header
    // comment) whenever DSDcc didn't hand back that much real decoded audio on its own; if it
    // handed back *more* (a genuine decode burst), let the queue grow by the extra rather than
    // dropping any of it.
    size_t added1 = pending1_.size() - before1;
    size_t added2 = pending2_.size() - before2;
    if (added1 < static_cast<size_t>(itemsThisCall)) {
        pending1_.resize(pending1_.size() + (static_cast<size_t>(itemsThisCall) - added1), 0.0f);
    }
    if (added2 < static_cast<size_t>(itemsThisCall)) {
        pending2_.resize(pending2_.size() + (static_cast<size_t>(itemsThisCall) - added2), 0.0f);
    }
}

} // namespace sdrscan
