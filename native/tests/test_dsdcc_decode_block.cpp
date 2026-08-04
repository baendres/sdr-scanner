// Regression test for a real production outage: DsdccDecodeBlock used to only produce()
// however many audio items DSDcc happened to have decoded on a given call - zero, most of the
// time, since real voice traffic is intermittent and DSDcc first has to sync to a frame. That
// fed ScanWindowBlock::mixerAdd_ (a synchronous gr::blocks::add_ff, see ScanWindow.cpp), which
// can't produce ANY output until every one of its connected ports has data - so a DMR/P25
// channel with no active traffic silently stalled the *entire* window's audio, not just its
// own, for as long as it wasn't mid-voice-frame (observed on real hardware as AudioMixer
// reporting ~100% starvation continuously on every receiver, and every channel's RSSI/volume/
// noise-floor telemetry frozen, even for unrelated FM channels sharing a window with a DMR
// channel - see native/README.md). Fixed by always producing a fixed-rate output stream
// (zero-filled when nothing's decoded yet), matching how every other channel mode's audio gate
// already behaves when squelched.

#include <catch2/catch_test_macros.hpp>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/noise_source.h>
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/vector_sink.h>
#include <gnuradio/sptr_magic.h>

#include "../src/dsp/DsdccDecodeBlock.h"

using namespace sdrscan;

TEST_CASE("DsdccDecodeBlock: produces a continuous fixed-rate output stream even with no real "
          "DMR sync (pure noise input)") {
    auto decode = gnuradio::make_block_sptr<DsdccDecodeBlock>(DSDcc::DSDDecoder::DSDDecodeDMR,
                                                                /*tdmaStereo=*/true);

    // Low-level noise, well below anything that could plausibly sync DSDcc's DMR frame
    // detector, so pending1_/pending2_ (the queues of actually-decoded audio) should stay
    // empty for this whole run - any output produced has to come from zero-fill.
    auto noise = gr::analog::noise_source_f::make(gr::analog::GR_GAUSSIAN, 0.02, 42);
    constexpr int kDiscriminatorRate = 48000;
    auto head = gr::blocks::head::make(sizeof(float), kDiscriminatorRate); // 1 second

    auto sink1 = gr::blocks::vector_sink_f::make();
    auto sink2 = gr::blocks::vector_sink_f::make();

    auto tb = gr::make_top_block("test-dsdcc-continuous-output");
    tb->connect(noise, 0, head, 0);
    tb->connect(head, 0, decode, 0);
    tb->connect(decode, 0, sink1, 0);
    tb->connect(decode, 1, sink2, 0);

    REQUIRE_NOTHROW(tb->run());

    // Both output ports must have produced a substantial, non-trivial number of items - not
    // zero, and not just a handful from a single lucky call - proving output kept flowing at a
    // steady rate for the whole 1-second run rather than stalling whenever nothing was decoded.
    // At the 48000/8000 = 6:1 input:output ratio, 1 second of input should yield ~8000 items;
    // allow generous slack for scheduler chunking, but this would be near-zero under the old
    // bursty-only-on-decode behavior since noise never syncs to a real DMR frame.
    CHECK(sink1->data().size() > 4000);
    CHECK(sink2->data().size() > 4000);
}
