// End-to-end CTCSS test: actually FM-modulates a baseband signal (voice-band tone + a
// sub-audible CTCSS tone) into a complex carrier, same as a real transmitter would, and
// checks ChannelBlockFM only unmutes when the configured tone matches what's embedded in the
// signal. No SDR hardware needed - this is the closest thing to an over-the-air test we can
// run in CI.

#include <catch2/catch_test_macros.hpp>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/sig_source.h>
#include <gnuradio/analog/frequency_modulator_fc.h>
#include <gnuradio/blocks/add_blk.h>
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>

#include <cmath>

#include "../src/dsp/ChannelBlockFM.h"

using namespace sdrscan;

namespace {

constexpr int kRfSampleRate = 240'000;
constexpr int kAudioSampleRate = 16'000;
constexpr int kDeviationHz = 2500; // NFM

// Builds and runs an FM-modulated test signal (voice tone + optional CTCSS sub-tone)
// through a ChannelBlockFM configured to require `requiredToneHz` (nullopt = disabled), and
// returns the resulting status.
ChannelStatus runTrial(double embeddedCtcssToneHz, std::optional<double> requiredToneHz) {
    auto tb = gr::make_top_block("test-ctcss");

    // Baseband: a 1kHz "voice" tone at 50% of max deviation, plus the CTCSS sub-tone at 15%.
    auto voiceTone = gr::analog::sig_source_f::make(kRfSampleRate, gr::analog::GR_COS_WAVE, 1000.0, 0.5);
    auto ctcssTone = gr::analog::sig_source_f::make(kRfSampleRate, gr::analog::GR_COS_WAVE, embeddedCtcssToneHz, 0.15);
    auto adder = gr::blocks::add_ff::make(1);

    float sensitivity = static_cast<float>(2.0 * M_PI * kDeviationHz / kRfSampleRate);
    auto modulator = gr::analog::frequency_modulator_fc::make(sensitivity);

    auto head = gr::blocks::head::make(sizeof(gr_complex), kRfSampleRate / 2); // 0.5s - CTCSS needs settling time
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*squelchThreshold=*/-40.0, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
        /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, kRfSampleRate, kAudioSampleRate,
        kDeviationHz, requiredToneHz, [](ChannelStatusUpdate) {});

    tb->connect(voiceTone, 0, adder, 0);
    tb->connect(ctcssTone, 0, adder, 1);
    tb->connect(adder, 0, modulator, 0);
    tb->connect(modulator, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    tb->run();

    return channel->getStatus();
}

} // namespace

TEST_CASE("CTCSS squelch opens when the embedded tone matches") {
    CHECK(runTrial(/*embeddedCtcssToneHz=*/100.0, /*requiredToneHz=*/100.0) == ChannelStatus::ACTIVE);
}

TEST_CASE("CTCSS squelch stays closed when the embedded tone doesn't match") {
    // 100.0 and 151.4 are both standard CTCSS tones, comfortably outside each other's
    // narrowband detection filter.
    CHECK(runTrial(/*embeddedCtcssToneHz=*/151.4, /*requiredToneHz=*/100.0) == ChannelStatus::IDLE);
}

TEST_CASE("Disabled CTCSS (nullopt) passes regardless of embedded tone") {
    CHECK(runTrial(/*embeddedCtcssToneHz=*/151.4, /*requiredToneHz=*/std::nullopt) == ChannelStatus::ACTIVE);
}

TEST_CASE("setCtcssTone changes the required tone live") {
    auto tb = gr::make_top_block("test-ctcss-live");
    auto voiceTone = gr::analog::sig_source_f::make(kRfSampleRate, gr::analog::GR_COS_WAVE, 1000.0, 0.5);
    auto ctcssTone = gr::analog::sig_source_f::make(kRfSampleRate, gr::analog::GR_COS_WAVE, 100.0, 0.15);
    auto adder = gr::blocks::add_ff::make(1);
    float sensitivity = static_cast<float>(2.0 * M_PI * kDeviationHz / kRfSampleRate);
    auto modulator = gr::analog::frequency_modulator_fc::make(sensitivity);
    auto head = gr::blocks::head::make(sizeof(gr_complex), kRfSampleRate / 2);
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockFM>(
        "test-channel", "Test", false, std::nullopt, false, -40.0, 0.0, 3.0, 0, 0,
        kRfSampleRate, kAudioSampleRate, kDeviationHz, /*ctcssToneHz=*/151.4 /* mismatched */,
        [](ChannelStatusUpdate) {});

    // Live-update to the matching tone before running - this is exactly the "hot update"
    // path the control API uses (see native/README.md).
    channel->setCtcssTone(100.0);

    tb->connect(voiceTone, 0, adder, 0);
    tb->connect(ctcssTone, 0, adder, 1);
    tb->connect(adder, 0, modulator, 0);
    tb->connect(modulator, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);
    tb->run();

    CHECK(channel->getStatus() == ChannelStatus::ACTIVE);
}
