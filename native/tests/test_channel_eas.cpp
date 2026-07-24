// Synthetic-signal tests for ChannelBlockEAS - no SDR hardware needed. FM-modulates one or
// more alert tones into a complex carrier (same technique as test_ctcss_squelch.cpp) and
// checks the tone-detect trigger latch opens after enough consecutive triggers, and stays
// closed when the tone(s) are absent, don't match, or are only partially present.

#include <catch2/catch_test_macros.hpp>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/sig_source.h>
#include <gnuradio/analog/frequency_modulator_fc.h>
#include <gnuradio/blocks/add_blk.h>
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>

#include <cmath>
#include <vector>

#include "../src/dsp/ChannelBlockEAS.h"

using namespace sdrscan;

namespace {

constexpr int kNoaaRfSampleRate = 240'000;
constexpr int kAudioSampleRate = 16'000;
constexpr int kNoaaDeviationHz = 5000;

constexpr int kBfmRfSampleRate = 2'400'000; // must divide evenly into a fmQuadRate multiple of kAudioSampleRate
constexpr int kBfmDeviationHz = 75000;

// Builds an FM-modulated test signal carrying `embeddedTonesHz` (summed equally) and runs it
// through a ChannelBlockEAS configured to look for `alertTonesHz`, for `durationSec` of RF
// time - long enough to cover the tone detector's 3-consecutive-trigger debounce plus filter
// settling. Returns the resulting status.
ChannelStatus runTrial(int rfSampleRate, int audioSampleRate, int deviationHz,
                        const std::vector<double>& embeddedTonesHz,
                        std::vector<double> alertTonesHz,
                        double durationSec = 1.0) {
    auto tb = gr::make_top_block("test-eas");

    auto adder = gr::blocks::add_ff::make(1);
    std::vector<gr::analog::sig_source_f::sptr> tones;
    for (size_t i = 0; i < embeddedTonesHz.size(); i++) {
        auto tone = gr::analog::sig_source_f::make(rfSampleRate, gr::analog::GR_COS_WAVE, embeddedTonesHz[i], 0.4);
        tones.push_back(tone);
        tb->connect(tone, 0, adder, static_cast<int>(i));
    }

    float sensitivity = static_cast<float>(2.0 * M_PI * deviationHz / rfSampleRate);
    auto modulator = gr::analog::frequency_modulator_fc::make(sensitivity);
    auto head = gr::blocks::head::make(sizeof(gr_complex), static_cast<uint64_t>(rfSampleRate * durationSec));
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockEAS>(
        "test-channel", "Test", /*mute=*/false, /*solo=*/std::nullopt, /*hold=*/false,
        /*squelchThreshold=*/-40.0, /*audioGain_dB=*/0.0, /*dwellTime_s=*/3.0,
        /*channelFreq_hz=*/0, /*hardwareFreq_hz=*/0, rfSampleRate, audioSampleRate,
        deviationHz, std::move(alertTonesHz), /*squelchNoiseMargin_dB=*/std::nullopt, [](ChannelStatusUpdate) {});

    tb->connect(adder, 0, modulator, 0);
    tb->connect(modulator, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);

    tb->run();

    return channel->getStatus();
}

} // namespace

TEST_CASE("ChannelBlockEAS (NOAA) goes ACTIVE when the SAME attention tone is present") {
    CHECK(runTrial(kNoaaRfSampleRate, kAudioSampleRate, kNoaaDeviationHz,
                    /*embeddedTonesHz=*/{1050.0}, /*alertTonesHz=*/{1050.0}) == ChannelStatus::ACTIVE);
}

TEST_CASE("ChannelBlockEAS (NOAA) stays IDLE when the SAME tone is absent") {
    CHECK(runTrial(kNoaaRfSampleRate, kAudioSampleRate, kNoaaDeviationHz,
                    /*embeddedTonesHz=*/{400.0}, /*alertTonesHz=*/{1050.0}) == ChannelStatus::IDLE);
}

TEST_CASE("ChannelBlockEAS (BFM_EAS) goes ACTIVE when both attention tones are present") {
    CHECK(runTrial(kBfmRfSampleRate, kAudioSampleRate, kBfmDeviationHz,
                    /*embeddedTonesHz=*/{853.0, 960.0}, /*alertTonesHz=*/{853.0, 960.0}) == ChannelStatus::ACTIVE);
}

TEST_CASE("ChannelBlockEAS (BFM_EAS) stays IDLE when only one of the two attention tones is present") {
    CHECK(runTrial(kBfmRfSampleRate, kAudioSampleRate, kBfmDeviationHz,
                    /*embeddedTonesHz=*/{853.0}, /*alertTonesHz=*/{853.0, 960.0}) == ChannelStatus::IDLE);
}

TEST_CASE("ChannelBlockEAS forceActive opens regardless of tone") {
    auto tb = gr::make_top_block("test-eas-force");
    auto tone = gr::analog::sig_source_f::make(kNoaaRfSampleRate, gr::analog::GR_COS_WAVE, 400.0, 0.4);
    float sensitivity = static_cast<float>(2.0 * M_PI * kNoaaDeviationHz / kNoaaRfSampleRate);
    auto modulator = gr::analog::frequency_modulator_fc::make(sensitivity);
    auto head = gr::blocks::head::make(sizeof(gr_complex), kNoaaRfSampleRate / 4);
    auto sink = gr::blocks::null_sink::make(sizeof(float));

    auto channel = gnuradio::make_block_sptr<ChannelBlockEAS>(
        "test-channel", "Test", false, std::nullopt, false, -40.0, 0.0, 3.0, 0, 0,
        kNoaaRfSampleRate, kAudioSampleRate, kNoaaDeviationHz, std::vector<double>{1050.0},
        /*squelchNoiseMargin_dB=*/std::nullopt, [](ChannelStatusUpdate) {});
    channel->setForceActive(true);

    tb->connect(tone, 0, modulator, 0);
    tb->connect(modulator, 0, head, 0);
    tb->connect(head, 0, channel, 0);
    tb->connect(channel, 0, sink, 0);
    tb->run();

    CHECK(channel->getStatus() == ChannelStatus::FORCE_ACTIVE);
}
