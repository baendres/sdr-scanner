// Verifies the core mechanism SoapyReceiver's window-hopping now depends on: that
// gr::blocks::selector can be redirected live, while the flowgraph keeps running, with no
// stop/restart - ported from Receiver.py's post-fork "one large block with all windows and
// selectors" rework (see the class comment on SoapyReceiver). No SDR hardware needed - this
// uses a synthetic source standing in for the real one, same approach as test_channel_fm.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <thread>

#include <gnuradio/top_block.h>
#include <gnuradio/analog/sig_source.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/selector.h>
#include <gnuradio/blocks/throttle.h>
#include <gnuradio/blocks/vector_sink.h>

namespace {
// Polls (rather than a single blind sleep-then-check) until the sink's most recent sample
// matches `expected`, or gives up after a generous timeout - avoids having to guess exactly how
// long a live switch takes to reach the sink, which varies with system/sandbox load.
bool waitForLatestSample(const gr::blocks::vector_sink_f::sptr& sink, float expected, float margin) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto& data = sink->data();
        if (!data.empty() && std::abs(data.back() - expected) <= margin) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("blocks::selector redirects both RF and audio sides live, no stop/restart") {
    auto tb = gr::make_top_block("test-selector-hop");

    // Stand-in for the hardware source: a constant-1.0 stream, same role as source_ in
    // SoapyReceiver - one signal, routed to whichever "window" is currently selected. Throttled
    // to a modest, realistic rate: a real SDR source paces itself to actual hardware throughput
    // (on the order of 1-2 Msps, not "as fast as the CPU can go") - an unthrottled synthetic
    // source is a pathological case real usage never hits, and let internal GNU Radio buffers
    // grow to match an observed throughput unrealistically large enough that draining a backlog
    // after a live switch became a genuine multi-second wait in this sandbox, not a fixed cost.
    auto src = gr::analog::sig_source_f::make(32000, gr::analog::GR_CONST_WAVE, 0, 1.0f);
    auto throttle = gr::blocks::throttle::make(sizeof(float), 32000);

    // Two "windows" distinguished by a different gain each, standing in for two different
    // ScanWindowBlocks. A third selector output (index 2) is the discard port, matching
    // SoapyReceiver's rfDiscardPortIndex_/rfDiscardSink_.
    auto rfSelector = gr::blocks::selector::make(sizeof(float), 0, 2);
    auto window0 = gr::blocks::multiply_const_ff::make(10.0f);
    auto window1 = gr::blocks::multiply_const_ff::make(20.0f);
    auto rfDiscardSink = gr::blocks::null_sink::make(sizeof(float));
    auto audioSelector = gr::blocks::selector::make(sizeof(float), 0, 0);
    auto sink = gr::blocks::vector_sink_f::make();

    tb->connect(src, 0, throttle, 0);
    tb->connect(throttle, 0, rfSelector, 0);
    tb->connect(rfSelector, 0, window0, 0);
    tb->connect(rfSelector, 1, window1, 0);
    tb->connect(rfSelector, 2, rfDiscardSink, 0);
    tb->connect(window0, 0, audioSelector, 0);
    tb->connect(window1, 0, audioSelector, 1);
    tb->connect(audioSelector, 0, sink, 0);

    tb->start();

    // Select window 0 (gain 10) and confirm real output actually starts flowing.
    audioSelector->set_input_index(0);
    rfSelector->set_output_index(0);
    REQUIRE(waitForLatestSample(sink, 10.0f, 0.5f));

    // Live-hop to window 1 (gain 20) - no stop()/wait()/reconnect anywhere here, exactly what
    // SoapyReceiver::startWindow() now does.
    audioSelector->set_input_index(1);
    rfSelector->set_output_index(1);
    bool sawWindow1 = waitForLatestSample(sink, 20.0f, 0.5f);

    tb->stop();
    tb->wait();

    CHECK(sawWindow1);
}
