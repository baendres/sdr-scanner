// Correctness tests for RfRingBuffer's wraparound math - the RF-rate bridge between
// SoapyReceiver's always-running capture flowgraph and its freely restartable per-window
// flowgraph (see native/src/receiver/RfRingBuffer.h). No SDR hardware needed.

#include <catch2/catch_test_macros.hpp>

#include "../src/receiver/RfRingBuffer.h"

using namespace sdrscan;

TEST_CASE("RfRingBuffer round-trips a simple write/read") {
    RfRingBuffer ring(16);

    std::vector<gr_complex> in = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
    ring.write(in.data(), static_cast<int>(in.size()));

    std::vector<gr_complex> out(4);
    int n = ring.read(out.data(), 4);
    REQUIRE(n == 4);
    for (int i = 0; i < 4; i++) CHECK(out[i] == in[i]);
}

TEST_CASE("RfRingBuffer read returns 0 when empty") {
    RfRingBuffer ring(16);
    std::vector<gr_complex> out(4);
    CHECK(ring.read(out.data(), 4) == 0);
}

TEST_CASE("RfRingBuffer partial reads leave the remainder for the next read") {
    RfRingBuffer ring(16);
    std::vector<gr_complex> in = {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}};
    ring.write(in.data(), static_cast<int>(in.size()));

    std::vector<gr_complex> out(3);
    REQUIRE(ring.read(out.data(), 3) == 3);
    CHECK(out[0] == in[0]);
    CHECK(out[2] == in[2]);

    REQUIRE(ring.read(out.data(), 3) == 2);
    CHECK(out[0] == in[3]);
    CHECK(out[1] == in[4]);
}

TEST_CASE("RfRingBuffer correctly wraps around the underlying storage") {
    // Small capacity forces multiple wraps quickly - exactly the arithmetic most likely to
    // have an off-by-one bug.
    RfRingBuffer ring(4);

    std::vector<gr_complex> readBack;
    for (int cycle = 0; cycle < 20; cycle++) {
        gr_complex sample(static_cast<float>(cycle), static_cast<float>(-cycle));
        ring.write(&sample, 1);

        gr_complex out;
        int n = ring.read(&out, 1);
        REQUIRE(n == 1);
        CHECK(out == sample);
    }
}

TEST_CASE("RfRingBuffer preserves order across many wraps with varying chunk sizes") {
    // write() blocks (sleeping) when full, and this test drives write/read sequentially on
    // one thread - so readChunk must stay >= writeChunk (net backlog never grows) or a full
    // buffer would block forever waiting for a reader that will never come on this thread.
    // capacity(8) vs 500 total items still forces ~60 wraps of the underlying storage.
    RfRingBuffer ring(8);

    std::vector<gr_complex> expected;
    for (int i = 0; i < 500; i++) expected.push_back(gr_complex(static_cast<float>(i), 0.0f));

    std::vector<gr_complex> actual;
    size_t writePos = 0;
    int writeChunk = 3;
    int readChunk = 5;

    while (actual.size() < expected.size()) {
        if (writePos < expected.size()) {
            int n = std::min<int>(writeChunk, static_cast<int>(expected.size() - writePos));
            ring.write(&expected[writePos], n);
            writePos += n;
        }
        gr_complex buf[8];
        int n = ring.read(buf, std::min<int>(readChunk, 8));
        for (int i = 0; i < n; i++) actual.push_back(buf[i]);
    }

    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
        CHECK(actual[i] == expected[i]);
    }
}
