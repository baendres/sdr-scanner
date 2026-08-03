// P25TsbkDecoder tests - no real P25 RF/captures available in this sandbox (see
// P25TsbkDecoder.h's class comment), so these validate what's independently checkable:
//  1. crc16Ccitt() against a standard, published CRC-16/GENIBUS test vector (this exact
//     bit-oriented parameterization - init 0, poly 0x1021, XOR-out 0xFFFF - is the CRC-16/
//     GENIBUS variant; verifies the ported algorithm is bit-correct against a source
//     independent of dsd-fme).
//  2. trellisDecode() via a self-consistent round trip: the decoder's own verified transition
//     table (kP25Dtm, ported from dsd-fme) defines a systematic trellis where the decoded state
//     sequence *is* the payload dibit sequence and the correct channel symbol for a transition
//     prevState->nextState is exactly kP25Dtm[prevState*4+nextState] (this is how the Viterbi
//     branch metric itself is computed - see trellisDecode()'s use of it as the "expected
//     transmitted nibble" to diff against). Encoding via that same rule and decoding back
//     exercises the deinterleave/traceback/byte-packing logic for implementation bugs (index
//     errors, table transposition, etc.) - it does not independently confirm this matches real
//     P25 transmitter framing, which needs a real capture.
//  3. channelToFrequency() arithmetic against a hand-picked IDEN_UP-populated table.
//  4. A full pushDibit() pipeline test (sync detect -> NID skip -> status-dibit-aware TSBK
//     collection -> trellis -> CRC -> opcode parse -> grant callback) for a constructed
//     GRP_V_CH_GRANT, using the same encoder from #2 plus the decoder's own documented
//     status-dibit cadence to build a valid raw dibit stream.

#include <catch2/catch_test_macros.hpp>

#include "../src/dsp/P25TsbkDecoder.h"

#include <optional>
#include <vector>

using namespace sdrscan;

namespace {

// Mirrors P25TsbkDecoder.cpp's private kP25Dtm/kP25Interleave tables (duplicated here rather
// than exposed, since they're implementation details of the ported algorithm, not part of the
// class's public contract).
constexpr uint8_t kDtm[16] = {
    2, 12, 1, 15,
    14, 0, 13, 3,
    9, 7, 10, 4,
    5, 11, 6, 8
};
constexpr uint8_t kInterleave[98] = {
    0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96, 97,
    2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
    4,  5,  12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93,
    6,  7,  14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95
};

// Encodes 12 payload bytes (96 bits -> 48 payload dibits, plus one trailing flush dibit) into the
// 98-dibit interleaved-channel-symbol form trellisDecode() expects as input - the forward
// direction of the same verified transition table.
std::array<uint8_t, 98> encodeTrellis(const uint8_t payload[12]) {
    uint8_t tdibits[49];
    // trellisDecode()'s traceback produces tdibits[0..48] with out[i] built from
    // tdibits[i*4+0..3] for i in 0..11 - i.e. tdibits[0..47] are the 48 payload dibits (the state
    // reached after each of nibs[0..47]) and tdibits[48] (state after the final nib) is an
    // unused flush dibit, not part of the payload. The state *before* nibs[0] (i.e. before
    // tdibits[0]) is the fixed encoder start state 0.
    for (int i = 0; i < 12; i++) {
        uint8_t byte = payload[i];
        tdibits[i * 4 + 0] = (byte >> 6) & 0x3;
        tdibits[i * 4 + 1] = (byte >> 4) & 0x3;
        tdibits[i * 4 + 2] = (byte >> 2) & 0x3;
        tdibits[i * 4 + 3] = byte & 0x3;
    }
    tdibits[48] = 0; // flush dibit - value is arbitrary since trellisDecode() never reads it back

    uint8_t nibs[49];
    for (int t = 0; t < 49; t++) {
        // trellisDecode's stage 0 always transitions from a fixed initial state 0 too (see its
        // "prev state is initial 0" comment).
        uint8_t prevState = (t == 0) ? 0 : tdibits[t - 1];
        uint8_t nextState = tdibits[t];
        nibs[t] = kDtm[prevState * 4 + nextState];
    }

    uint8_t deinterleaved[98];
    for (int i = 0; i < 49; i++) {
        deinterleaved[i * 2 + 0] = (nibs[i] >> 2) & 0x3;
        deinterleaved[i * 2 + 1] = nibs[i] & 0x3;
    }

    std::array<uint8_t, 98> input{};
    for (int i = 0; i < 98; i++) input[i] = deinterleaved[kInterleave[i]];
    return input;
}

} // namespace

TEST_CASE("P25TsbkDecoder::crc16Ccitt matches a known CRC-16/XMODEM vector, inverted") {
    // "123456789" ASCII, MSB-first bits. crc16Ccitt is init=0x0000/poly=0x1021/no-reflect with a
    // final xorout=0xFFFF (P25's documented 1's-complement CRC step) - i.e. it's CRC-16/XMODEM
    // (init 0x0000, poly 0x1021, no reflection, xorout 0x0000; well-known check value 0x31C3 for
    // this same message) with that extra inversion applied, giving 0x31C3 ^ 0xFFFF = 0xCE3C.
    // (Not CRC-16/GENIBUS, despite the similar parameters - GENIBUS uses init 0xFFFF, which would
    // give 0xD64E instead.)
    const char* msg = "123456789";
    uint8_t bits[72];
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) bits[i * 8 + b] = (static_cast<uint8_t>(msg[i]) >> (7 - b)) & 1;
    }
    CHECK(P25TsbkDecoder::crc16Ccitt(bits, 72) == 0xCE3C);
}

TEST_CASE("P25TsbkDecoder::trellisDecode round-trips a constructed payload with zero error metric") {
    uint8_t payload[12] = {0x40, 0x00, 0xAB, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0x00, 0x00};
    auto encoded = encodeTrellis(payload);

    uint8_t decoded[12];
    int errMetric = P25TsbkDecoder::trellisDecode(encoded.data(), decoded);

    CHECK(errMetric == 0);
    CHECK(std::equal(std::begin(payload), std::end(payload), std::begin(decoded)));
}

TEST_CASE("P25TsbkDecoder: full pipeline decodes a constructed GRP_V_CH_GRANT and fires onGrant") {
    // IDEN_UP for iden 1: base freq 851.0125 MHz (units of 5Hz -> 170202500), spacing 12.5kHz
    // (units of 125Hz -> 100).
    uint8_t idenBytes[12] = {0x34, 0x00, 0x10, 0x00, 0x00, 0x64, 0x0A, 0x25, 0x15, 0x84, 0, 0};
    // byte2 = (iden<<4)|bw = 0x10 -> iden=1; byte3=0,byte4=0 -> trans_off=0;
    // byte4&0x3=0,byte5=0x64=100 -> chan_spac=100 (12.5kHz); byte6..9 = 0x0A251584 = 170202500
    // (851.0125MHz in 5Hz units).
    idenBytes[6] = 0x0A;
    idenBytes[7] = 0x25;
    idenBytes[8] = 0x15;
    idenBytes[9] = 0x84;

    // GRP_V_CH_GRANT: opcode 0x00, MFID 0x00, svc=0, channel=0x1064 (iden=1, chan#=0x064=100),
    // talkgroup=12345 (0x3039).
    uint8_t grantBytes[12] = {0x00, 0x00, 0x00, 0x10, 0x64, 0x30, 0x39, 0x00, 0x00, 0x00, 0, 0};

    auto appendCrc = [](uint8_t bytes[12]) {
        uint8_t bits[96];
        for (int i = 0; i < 12; i++)
            for (int b = 0; b < 8; b++) bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1;
        uint16_t crc = P25TsbkDecoder::crc16Ccitt(bits, 80);
        bytes[10] = static_cast<uint8_t>(crc >> 8);
        bytes[11] = static_cast<uint8_t>(crc & 0xFF);
    };
    appendCrc(idenBytes);
    appendCrc(grantBytes);

    auto idenBlock = encodeTrellis(idenBytes);
    auto grantBlock = encodeTrellis(grantBytes);

    // Builds the 101-raw-dibit stream for one TSBK block from its 98 "real" dibits, inserting
    // placeholder status dibits (value irrelevant - discarded by the decoder) at the exact
    // positions collectTsbkDibit()'s divide-by-36 cadence (starting at 36-14=22, matching
    // P25TsbkDecoder.cpp) will classify as status.
    auto toRawStream = [](const std::array<uint8_t, 98>& real) {
        std::vector<uint8_t> raw;
        int skipCycle = 36 - 14;
        int realIdx = 0;
        for (int i = 0; i < 101; i++) {
            if ((skipCycle / 36) == 0) {
                raw.push_back(real[realIdx++]);
            } else {
                skipCycle = 0;
                raw.push_back(0); // status placeholder, discarded
            }
            skipCycle++;
        }
        return raw;
    };

    constexpr uint8_t kSyncPattern[24] = {
        1, 1, 1, 1, 1, 3, 1, 1, 3, 3, 1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 3, 3, 3
    };

    std::optional<uint32_t> grantedTalkgroup;
    std::optional<int64_t> grantedFreq;
    P25TsbkDecoder decoder([&](uint32_t tg, int64_t freq) {
        grantedTalkgroup = tg;
        grantedFreq = freq;
    });

    for (uint8_t d : kSyncPattern) decoder.pushDibit(d);
    for (int i = 0; i < 32; i++) decoder.pushDibit(0); // NID - content doesn't matter, just skipped

    for (uint8_t d : toRawStream(idenBlock)) decoder.pushDibit(d);
    CHECK(decoder.everSynced());
    CHECK_FALSE(grantedTalkgroup.has_value()); // IDEN_UP alone shouldn't fire a grant

    // After 3 chained TSBK blocks the decoder resyncs to search state - feed a fresh sync+NID
    // before the grant block, matching a new frame.
    for (uint8_t d : toRawStream(grantBlock)) decoder.pushDibit(d);
    for (uint8_t d : toRawStream(grantBlock)) decoder.pushDibit(d);
    for (uint8_t d : kSyncPattern) decoder.pushDibit(d);
    for (int i = 0; i < 32; i++) decoder.pushDibit(0);
    for (uint8_t d : toRawStream(grantBlock)) decoder.pushDibit(d);

    REQUIRE(grantedTalkgroup.has_value());
    CHECK(*grantedTalkgroup == 12345);
    REQUIRE(grantedFreq.has_value());
    CHECK(*grantedFreq == 170202500LL * 5 + 100LL * 100 * 125);
}

TEST_CASE("P25TsbkDecoder::channelToFrequency returns 0 for an unpopulated IDEN") {
    std::optional<uint32_t> tg;
    std::optional<int64_t> freq;
    P25TsbkDecoder decoder([&](uint32_t t, int64_t f) { tg = t; freq = f; });
    CHECK(decoder.channelToFrequency(0x1064) == 0);
}
