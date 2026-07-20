// Tests for the Icecast audio output's pure-logic pieces (URL parsing, base64 encoding) - no
// network/server needed. The streaming/encoding thread itself needs a real Icecast server to
// meaningfully exercise and isn't covered here.

#include <catch2/catch_test_macros.hpp>

#include "../src/audio/AudioOutputIcecast.h"
#include "../src/util/Base64.h"

using namespace sdrscan;

TEST_CASE("base64Encode matches the RFC 4648 test vectors") {
    CHECK(base64Encode("") == "");
    CHECK(base64Encode("f") == "Zg==");
    CHECK(base64Encode("fo") == "Zm8=");
    CHECK(base64Encode("foo") == "Zm9v");
    CHECK(base64Encode("foob") == "Zm9vYg==");
    CHECK(base64Encode("fooba") == "Zm9vYmE=");
    CHECK(base64Encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64Encode matches a known source:password Basic-auth value") {
    // Independently verified: base64("source:hackme") == "c291cmNlOmhhY2ttZQ=="
    CHECK(base64Encode("source:hackme") == "c291cmNlOmhhY2ttZQ==");
}

TEST_CASE("parseIcecastUrl handles host/port/mount") {
    auto parts = parseIcecastUrl("http://stream.example.com:8000/mystream");
    CHECK(parts.host == "stream.example.com");
    CHECK(parts.port == 8000);
    CHECK(parts.mount == "/mystream");
}

TEST_CASE("parseIcecastUrl defaults to port 80 when omitted") {
    auto parts = parseIcecastUrl("http://stream.example.com/mystream");
    CHECK(parts.host == "stream.example.com");
    CHECK(parts.port == 80);
    CHECK(parts.mount == "/mystream");
}

TEST_CASE("parseIcecastUrl defaults the mount to / when omitted") {
    auto parts = parseIcecastUrl("http://stream.example.com:8000");
    CHECK(parts.host == "stream.example.com");
    CHECK(parts.port == 8000);
    CHECK(parts.mount == "/");
}

TEST_CASE("parseIcecastUrl accepts a bare host:port/mount with no scheme") {
    auto parts = parseIcecastUrl("stream.example.com:8000/mystream");
    CHECK(parts.host == "stream.example.com");
    CHECK(parts.port == 8000);
    CHECK(parts.mount == "/mystream");
}

TEST_CASE("parseIcecastUrl rejects a non-http scheme") {
    CHECK_THROWS_AS(parseIcecastUrl("https://stream.example.com/mystream"), std::runtime_error);
    CHECK_THROWS_AS(parseIcecastUrl("ftp://stream.example.com/mystream"), std::runtime_error);
}

TEST_CASE("parseIcecastUrl rejects a missing host") {
    CHECK_THROWS_AS(parseIcecastUrl("http:///mystream"), std::runtime_error);
}

TEST_CASE("AudioOutputIcecast construction validates the URL up front") {
    CHECK_NOTHROW(AudioOutputIcecast("http://stream.example.com:8000/mystream", "hackme"));
    CHECK_THROWS_AS(AudioOutputIcecast("https://stream.example.com/mystream", ""), std::runtime_error);
}

TEST_CASE("AudioOutputIcecast::send bounds its buffer like a maxlen deque") {
    // Mirrors AudioServer.py's collections.deque(maxlen=SAMPLES_PER_FRAME*3) - overflowing
    // send() calls should drop the oldest samples, not grow unbounded or throw.
    AudioOutputIcecast out("http://stream.example.com:8000/mystream", "hackme");
    std::vector<int16_t> chunk(20000, 1);
    CHECK_NOTHROW(out.send(chunk));
    CHECK_NOTHROW(out.send(chunk));
}
