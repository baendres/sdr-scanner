#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace sdrscan {

// Standalone P25 Phase 1 control-channel TSBK (Trunking Signal Block) decoder - not DSDcc-based
// (DSDcc has no trunking decode at all, only post-sync voice-frame heuristics - see
// native/README.md). Takes a stream of raw dibits (0-3, one per received P25 C4FM symbol, fed by
// ChannelBlockP25Control's own symbol-timing-recovery + slicer front end) and, once synced,
// decodes TSBK blocks, tracking the IDEN_UP frequency-band table and reporting voice channel
// grants for talkgroups via a callback.
//
// The bit-level algorithms below (98-dibit 1/2-rate trellis Viterbi decode, CRC-16/CCITT, the
// channel-to-frequency formula) are ported from lwvmobile/dsd-fme (GPL), an actively maintained
// fork that - unlike this app's existing DSDcc dependency - actually implements P25 trunking
// decode; verified by reading its source (src/p25_12.c, src/p25_crc.c, src/p25_frequency.c)
// rather than derived from memory, since this is the highest-risk, least-verifiable part of the
// whole P25 feature (no real P25 RF or captures available to test against in this sandbox).
// Standard (non-vendor) TSBK opcode field layouts (Group Voice Channel Grant/Update, IDEN_UP)
// were cross-checked against dsd-fme's own field-extraction code (src/p25p2_vpdu.c's
// process_MAC_VPDU, which handles both P25p2 MAC PDUs and - via a documented "opcode | 0x40"
// remap - legacy P25p1 TSBK content) rather than trusted purely from memory.
//
// Exact frame-timing alignment (where within the raw dibit stream the NID ends and TSBK data
// begins, and where periodic status-symbol dibits fall) is a best-effort port of dsd-fme's own
// verified dibit-accounting, not independently re-derived from the TIA-102 spec (which wasn't
// available to consult directly) - if it's slightly off, every TSBK block simply fails its
// CRC-16 check and gets silently discarded (safe failure: no grant is ever reported from
// unvalidated data), not a source of incorrect talkgroup/frequency output.
class P25TsbkDecoder {
public:
    // talkgroupId: the group address from a Group Voice Channel Grant(/Update). freqHz: the
    // resolved voice channel frequency (0 if the referenced IDEN hasn't been seen yet via
    // IDEN_UP, in which case the grant should be ignored - the frequency table isn't ready).
    using GrantCallback = std::function<void(uint32_t talkgroupId, int64_t freqHz)>;

    explicit P25TsbkDecoder(GrantCallback onGrant);

    // Feed one recovered symbol (0-3) at a time - see ChannelBlockP25Control.
    void pushDibit(uint8_t dibit);

    // True once at least one TSBK block has passed CRC since construction - used for
    // ChannelStatus (ACTIVE while the decoder is genuinely synced to the control channel).
    bool everSynced() const { return everSynced_; }

    // --- Exposed for unit testing independent of the dibit-stream sync/framing front end above
    // (see native/tests/test_p25_tsbk_decoder.cpp) ---

    // Ported from dsd-fme's p25_12() (src/p25_12.c): deinterleaves 98 input dibits and runs a
    // 4-state Viterbi decode of P25's 1/2-rate trellis code, writing 12 decoded bytes to
    // `out` and returning the best path's Hamming-distance error metric (0 = clean decode).
    static int trellisDecode(const uint8_t input[98], uint8_t out[12]);

    // Ported from dsd-fme's ComputeCrcCCITT16b() (src/p25_crc.c): CRC-16/CCITT, poly 0x1021,
    // result inverted. `bits` holds one bit per byte (0 or 1), matching the ported algorithm's
    // original bit-oriented interface.
    static uint16_t crc16Ccitt(const uint8_t bits[], unsigned int len);

    // Ported formula from dsd-fme's process_channel_to_freq() (src/p25_frequency.c). Returns 0
    // if the channel's IDEN hasn't been populated by an IDEN_UP message yet.
    int64_t channelToFrequency(uint16_t channel) const;

private:
    enum class State { SearchingSync, SkippingNid, CollectingTsbk };

    void onSymbolSynced();
    void collectTsbkDibit(uint8_t dibit);
    void processTsbkBlock(const uint8_t dibits[98]);
    void handleOpcode(const uint8_t bytes[12]);
    void handleIdenUp(const uint8_t bytes[12]);
    void handleGroupVoiceChannelGrant(const uint8_t bytes[12]);
    void handleGroupVoiceChannelGrantUpdate(const uint8_t bytes[12]);

    GrantCallback onGrant_;
    bool everSynced_ = false;

    State state_ = State::SearchingSync;
    uint64_t syncHistory_ = 0; // rolling 2-bit-per-dibit shift register, last 24 dibits (48 bits)
    int dibitsSeenInSearch_ = 0; // guards against matching before 24 dibits have been seen at all

    int nidDibitsRemaining_ = 0; // NID region skip (see class header's alignment note)

    // TSBK block collection - up to 3 chained blocks per message, 101 raw dibits per block (98
    // real + periodic status-symbol dibits discarded), same shape as dsd-fme's processTSBK loop.
    int repIndex_ = 0;
    int rawDibitIndex_ = 0;
    int skipdibitCycle_ = 0; // dsd-fme's verified status-dibit cadence counter, see .cpp
    uint8_t tsbkDibits_[98];
    int tsbkDibitCount_ = 0;

    struct IdenEntry {
        bool valid = false;
        int type = 1; // 1 = FDMA (only type this decoder populates - see IDEN_UP handling)
        int64_t baseFreqHz = 0;
        int64_t channelSpacingHz = 0;
    };
    std::array<IdenEntry, 16> idenTable_;
};

} // namespace sdrscan
