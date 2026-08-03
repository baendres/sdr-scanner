#include "P25TsbkDecoder.h"

#include <algorithm>
#include <cstring>

namespace sdrscan {

namespace {

// --- Ported from dsd-fme's src/p25_12.c (P25p1 1/2-rate trellis decoder) ---

constexpr uint8_t kP25Interleave[98] = {
    0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96, 97,
    2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
    4,  5,  12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93,
    6,  7,  14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95
};

// Dibit-pair -> trellis transition-matrix table (SDRTrunk/Ossmann convention, via dsd-fme).
constexpr uint8_t kP25Dtm[16] = {
    2,  12, 1,  15,
    14, 0,  13, 3,
    9,  7,  10, 4,
    5,  11, 6,  8
};

int countBits(uint8_t b, int slen) {
    int c = 0;
    for (int j = 0; j < slen; j++) {
        if (b & 1) c++;
        b >>= 1;
    }
    return c;
}

// P25 Phase 1 frame sync (verified against DSDcc's own m_syncPatterns[SyncP25P1] table, dibit
// values in the 0=+1/1=+3/2=-1/3=-3 OTA-symbol convention - see native/README.md). Tolerance of
// 2 mismatching dibits, matching DSDcc's own m_syncLenTol for this pattern.
constexpr uint8_t kSyncPattern[24] = {
    1, 1, 1, 1, 1, 3, 1, 1, 3, 3, 1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 3, 3, 3
};
constexpr int kSyncToleranceMismatches = 2;

uint64_t packSyncPattern() {
    uint64_t v = 0;
    for (uint8_t d : kSyncPattern) v = (v << 2) | d;
    return v;
}

int countDibitMismatches(uint64_t a, uint64_t b) {
    int mismatches = 0;
    for (int i = 0; i < 24; i++) {
        if (((a >> (2 * i)) & 0x3) != ((b >> (2 * i)) & 0x3)) mismatches++;
    }
    return mismatches;
}

} // namespace

P25TsbkDecoder::P25TsbkDecoder(GrantCallback onGrant) : onGrant_(std::move(onGrant)) {}

int P25TsbkDecoder::trellisDecode(const uint8_t input[98], uint8_t out[12]) {
    uint8_t deinterleaved[98];
    for (int i = 0; i < 98; i++) deinterleaved[kP25Interleave[i]] = input[i];

    uint8_t nibs[49];
    for (int i = 0; i < 49; i++) {
        nibs[i] = static_cast<uint8_t>((deinterleaved[i * 2 + 0] << 2) | deinterleaved[i * 2 + 1]);
    }

    // Viterbi: 4 states, 49 stages. UINT8_MAX/2 used as an "infinity" proxy (max possible
    // per-path error is ~196), same as the ported source.
    constexpr uint8_t kInf = 255 / 2;
    uint8_t metric[2][4];
    uint8_t path[49][4];
    std::memset(path, 0, sizeof(path));

    int curr = 0, prev = 1;
    for (int nextS = 0; nextS < 4; nextS++) {
        int branch = countBits(nibs[0] ^ kP25Dtm[0 * 4 + nextS], 4);
        metric[curr][nextS] = static_cast<uint8_t>(branch);
        path[0][nextS] = 0;
    }

    for (int t = 1; t < 49; t++) {
        prev = curr;
        curr = 1 - curr;
        for (int nextS = 0; nextS < 4; nextS++) metric[curr][nextS] = kInf;

        for (int nextS = 0; nextS < 4; nextS++) {
            uint8_t minDist = kInf;
            uint8_t bestPrev = 0;
            for (int prevS = 0; prevS < 4; prevS++) {
                if (metric[prev][prevS] == kInf) continue;
                int branch = countBits(nibs[t] ^ kP25Dtm[prevS * 4 + nextS], 4);
                uint8_t cand = static_cast<uint8_t>(metric[prev][prevS] + branch);
                if (cand < minDist) {
                    minDist = cand;
                    bestPrev = static_cast<uint8_t>(prevS);
                }
            }
            metric[curr][nextS] = minDist;
            path[t][nextS] = bestPrev;
        }
    }

    uint8_t minFinal = 0;
    uint8_t minMetric = metric[curr][0];
    for (int s = 1; s < 4; s++) {
        if (metric[curr][s] < minMetric) {
            minMetric = metric[curr][s];
            minFinal = static_cast<uint8_t>(s);
        }
    }

    uint8_t tdibits[49];
    tdibits[48] = minFinal;
    for (int t = 48; t >= 1; t--) tdibits[t - 1] = path[t][tdibits[t]];

    for (int i = 0; i < 12; i++) {
        out[i] = static_cast<uint8_t>((tdibits[i * 4 + 0] << 6) | (tdibits[i * 4 + 1] << 4) |
                                       (tdibits[i * 4 + 2] << 2) | tdibits[i * 4 + 3]);
    }

    return static_cast<int>(minMetric);
}

// --- Ported from dsd-fme's src/p25_crc.c (ComputeCrcCCITT16b) ---
uint16_t P25TsbkDecoder::crc16Ccitt(const uint8_t bits[], unsigned int len) {
    uint16_t crc = 0x0000;
    constexpr uint16_t kPoly = 0x1021;
    for (unsigned int i = 0; i < len; i++) {
        if (((crc >> 15) & 1) ^ (bits[i] & 1)) {
            crc = static_cast<uint16_t>((crc << 1) ^ kPoly);
        } else {
            crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc ^ 0xFFFF;
}

// --- Ported formula from dsd-fme's src/p25_frequency.c (process_channel_to_freq) ---
int64_t P25TsbkDecoder::channelToFrequency(uint16_t channel) const {
    if (channel == 0xFFFF) return 0;
    int iden = channel >> 12;
    const IdenEntry& entry = idenTable_[iden];
    if (!entry.valid) return 0;
    // slotsPerCarrier is 1 for FDMA (type 1, the only type this decoder's IDEN_UP handling
    // populates - see the class header's v1 scope note), so step is the raw 12-bit channel
    // number with no division needed.
    int64_t step = channel & 0xFFF;
    return entry.baseFreqHz + step * entry.channelSpacingHz;
}

void P25TsbkDecoder::pushDibit(uint8_t dibit) {
    dibit &= 0x3;

    switch (state_) {
        case State::SearchingSync: {
            syncHistory_ = ((syncHistory_ << 2) | dibit) & 0xFFFFFFFFFFFFULL; // 48 bits = 24 dibits
            if (dibitsSeenInSearch_ < 24) dibitsSeenInSearch_++;
            // Check as soon as the 24-dibit window is full (dibitsSeenInSearch_ just reached 24)
            // rather than skipping that dibit and starting the check one dibit late - otherwise a
            // cleanly aligned sync pattern is never actually detected, only ever a stale,
            // one-dibit-shifted window on the following dibit.
            if (dibitsSeenInSearch_ >= 24 &&
                countDibitMismatches(syncHistory_, packSyncPattern()) <= kSyncToleranceMismatches) {
                state_ = State::SkippingNid;
                nidDibitsRemaining_ = 32; // NID = 64 bits = 32 dibits (see class header's caveat)
            }
            break;
        }
        case State::SkippingNid:
            nidDibitsRemaining_--;
            if (nidDibitsRemaining_ <= 0) {
                state_ = State::CollectingTsbk;
                repIndex_ = 0;
                rawDibitIndex_ = 0;
                // Verified initial cadence ported from dsd-fme's processTSBK (src/p25p1_tsbk.c) -
                // see this class's header for the alignment caveat.
                skipdibitCycle_ = 36 - 14;
                tsbkDibitCount_ = 0;
            }
            break;
        case State::CollectingTsbk:
            collectTsbkDibit(dibit);
            break;
    }
}

void P25TsbkDecoder::collectTsbkDibit(uint8_t dibit) {
    if ((skipdibitCycle_ / 36) == 0) {
        if (tsbkDibitCount_ < 98) tsbkDibits_[tsbkDibitCount_++] = dibit;
    } else {
        skipdibitCycle_ = 0;
    }
    skipdibitCycle_++;
    rawDibitIndex_++;

    if (rawDibitIndex_ >= 101) {
        processTsbkBlock(tsbkDibits_);
        tsbkDibitCount_ = 0;
        rawDibitIndex_ = 0;
        repIndex_++;
        if (repIndex_ >= 3) {
            state_ = State::SearchingSync;
            dibitsSeenInSearch_ = 0;
            syncHistory_ = 0;
        }
    }
}

void P25TsbkDecoder::processTsbkBlock(const uint8_t dibits[98]) {
    uint8_t bytes[12];
    trellisDecode(dibits, bytes);

    uint8_t bits[96];
    for (int i = 0; i < 12; i++) {
        for (int b = 0; b < 8; b++) bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1;
    }

    // CRC-16 covers the first 80 bits (10 bytes); the trailing 16 bits are the embedded CRC
    // itself - matches dsd-fme's crc16_lb_bridge(tsbk_decoded_bits, 80).
    uint16_t computed = crc16Ccitt(bits, 80);
    uint16_t embedded = 0;
    for (int i = 0; i < 16; i++) embedded = static_cast<uint16_t>((embedded << 1) | bits[80 + i]);

    if (computed != embedded) return; // CRC failed - discard (see class header's safety note)

    everSynced_ = true;
    handleOpcode(bytes);
}

void P25TsbkDecoder::handleOpcode(const uint8_t bytes[12]) {
    uint8_t opcode = bytes[0] & 0x3F;
    uint8_t mfid = bytes[1];
    // Vendor-specific (MFID >= 2) opcodes are out of scope for v1 - see class header.
    if (mfid > 0x01) return;

    switch (opcode) {
        case 0x00: handleGroupVoiceChannelGrant(bytes); break;
        case 0x02: handleGroupVoiceChannelGrantUpdate(bytes); break;
        case 0x34: handleIdenUp(bytes); break;
        default: break;
    }
}

void P25TsbkDecoder::handleIdenUp(const uint8_t bytes[12]) {
    // Field layout verified against dsd-fme's src/p25p2_vpdu.c (process_MAC_VPDU, opcode 0x74 =
    // TSBK opcode 0x34 with the standard "| 0x40" MAC-compat bit set - see class header).
    int iden = bytes[2] >> 4;
    int64_t chanSpacingUnits = ((bytes[4] & 0x3) << 8) | bytes[5];
    int64_t baseFreqUnits = (static_cast<int64_t>(bytes[6]) << 24) | (static_cast<int64_t>(bytes[7]) << 16) |
                             (static_cast<int64_t>(bytes[8]) << 8) | bytes[9];

    IdenEntry& entry = idenTable_[iden];
    entry.valid = true;
    entry.type = 1; // FDMA only - v1 doesn't decode the TDMA IDEN_UP variant (opcode 0x33)
    entry.baseFreqHz = baseFreqUnits * 5;         // base freq is in units of 5Hz
    entry.channelSpacingHz = chanSpacingUnits * 125; // spacing is in units of 125Hz
}

void P25TsbkDecoder::handleGroupVoiceChannelGrant(const uint8_t bytes[12]) {
    // Field layout verified against dsd-fme's process_MAC_VPDU, opcode 0x40 (= TSBK 0x00).
    uint16_t channel = static_cast<uint16_t>((bytes[3] << 8) | bytes[4]);
    uint32_t talkgroup = static_cast<uint32_t>((bytes[5] << 8) | bytes[6]);

    int64_t freq = channelToFrequency(channel);
    if (freq != 0 && onGrant_) onGrant_(talkgroup, freq);
}

void P25TsbkDecoder::handleGroupVoiceChannelGrantUpdate(const uint8_t bytes[12]) {
    // Field layout verified against dsd-fme's process_MAC_VPDU, opcode 0x42 (= TSBK 0x02) - a
    // single TSBK carries up to two independent channel/talkgroup grant refreshes.
    uint16_t channel1 = static_cast<uint16_t>((bytes[2] << 8) | bytes[3]);
    uint32_t talkgroup1 = static_cast<uint32_t>((bytes[4] << 8) | bytes[5]);
    uint16_t channel2 = static_cast<uint16_t>((bytes[6] << 8) | bytes[7]);
    uint32_t talkgroup2 = static_cast<uint32_t>((bytes[8] << 8) | bytes[9]);

    int64_t freq1 = channelToFrequency(channel1);
    if (freq1 != 0 && onGrant_) onGrant_(talkgroup1, freq1);

    if (channel2 != channel1 && channel2 != 0 && channel2 != 0xFFFF) {
        int64_t freq2 = channelToFrequency(channel2);
        if (freq2 != 0 && onGrant_) onGrant_(talkgroup2, freq2);
    }
}

} // namespace sdrscan
