#pragma once

#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AudioOutput.h"

namespace sdrscan {

// Streams raw 16-bit signed mono PCM @ AUDIO_SAMPLERATE to any number of connected WebSocket
// clients (e.g. the web UI's browser audio player). Direct port of AudioServer.py's
// AudioServerOutput_Websocket, using Boost.Beast (already a required GNU Radio build
// dependency) instead of Python's `websockets` package.
class AudioOutputWebsocket : public AudioOutput {
public:
    AudioOutputWebsocket(std::string host, int port);
    ~AudioOutputWebsocket() override;

    void reconnect() override;
    void close() override;
    void send(const std::vector<int16_t>& samples) override;

private:
    using WsStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    void acceptLoop();
    // Drains frameQueue_ and performs the actual (blocking) per-client socket writes - see the
    // comment on frameQueue_ for why this can't happen on AudioMixer's own thread.
    void writerLoop();

    std::string host_;
    int port_;

    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::thread acceptThread_;
    std::atomic<bool> stopFlag_{false};

    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<WsStream>> clients_;

    // AudioMixer calls send() roughly once per ms with whatever handful of samples
    // accumulated in that instant. Forwarding each of those as its own WS message produces
    // ~1000 tiny, irregularly-sized messages/sec - browsers schedule a separate audio buffer
    // node per incoming message, so that many small clock-drifted messages per second is a
    // direct cause of choppy playback. Buffer up to a ~250ms frame (matching the Python
    // version's AudioServerOutput_Websocket.SAMPLES_PER_FRAME, and this codebase's own
    // AudioOutputUdp) before actually writing to the socket.
    std::mutex outputBufferMutex_;
    std::vector<int16_t> outputBuffer_;

    // Completed frames land here (send() just pushes, never blocks); writerLoop() on its own
    // dedicated thread is what actually calls WsStream::write() - a real hardware crash-loop
    // traced back to that write happening directly on AudioMixer's own thread instead: it's a
    // synchronous socket write with no timeout configured, so one slow/unresponsive client (a
    // browser tab open but not reading, a laptop that went to sleep mid-connection, ...) could
    // block it indefinitely, freezing every receiver's audio at once and eventually tripping
    // the liveness watchdog. Matches AudioOutputIcecast's existing send()-buffers/thread-writes
    // split for the same reason.
    std::mutex frameQueueMutex_;
    std::vector<std::vector<int16_t>> frameQueue_;
    std::thread writerThread_;
};

} // namespace sdrscan
