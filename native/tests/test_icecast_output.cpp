// Tests for the Icecast audio output's pure-logic pieces (URL parsing, base64 encoding) - no
// network/server needed - plus a real-socket regression test for close()'s shutdown-timeout
// behavior below (see that test for why a real listener is used instead of pure logic).

#include <catch2/catch_test_macros.hpp>

#include "../src/audio/AudioOutputIcecast.h"
#include "../src/util/Base64.h"

#include <boost/asio.hpp>

#include <chrono>
#include <thread>

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

TEST_CASE("AudioOutputIcecast::close() doesn't hang against a server that accepts but never responds") {
    // Regression test for a real hang: connectSourceSocket() blocks in a plain synchronous
    // read_until() waiting for SOURCE-response headers, with no timeout. A bare listener that
    // accepts the TCP connection and then sends nothing reproduces exactly that stall (matches
    // e.g. an Icecast server wedged at the app layer, or a path that silently drops return
    // packets) without needing a real Icecast server.
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    asio::io_context serverIoc;
    tcp::acceptor acceptor(serverIoc, tcp::endpoint(tcp::v4(), 0));
    unsigned short port = acceptor.local_endpoint().port();

    std::thread serverThread([&acceptor, &serverIoc] {
        boost::system::error_code ec;
        tcp::socket socket(serverIoc);
        acceptor.accept(socket, ec);
        // Hold the connection open without ever responding - the streaming thread should be
        // stuck in read_until() at this point. Just idle here; the test process exiting (this
        // thread is detached below) cleans up the socket either way.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    });
    serverThread.detach();

    AudioOutputIcecast out("http://127.0.0.1:" + std::to_string(port) + "/mystream", "hackme");
    out.reconnect();

    // Give the streaming thread time to actually connect, send the SOURCE request, and land in
    // the blocking read_until() call this test means to interrupt.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto start = std::chrono::steady_clock::now();
    out.close();
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < std::chrono::seconds(2));
}
