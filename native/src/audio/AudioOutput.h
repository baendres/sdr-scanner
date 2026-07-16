#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../config/Types.h"

namespace sdrscan {

// One mixed audio stream (16-bit signed mono @ AUDIO_SAMPLERATE) fanned out to zero or more
// of these. Direct port of AudioServer.py's AudioServerOutput_Base hierarchy.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // (Re)establish the output (open device/socket/etc). Called once at startup and again
    // by implementations that need to recover from a dropped connection.
    virtual void reconnect() = 0;
    virtual void close() = 0;
    virtual void send(const std::vector<int16_t>& samples) = 0;
};

// Factory implemented in the AudioOutput*.cpp files (local | udp | websocket). Throws if
// `cfg.type` is unrecognized. (Icecast is a deferred follow-up - see native/README.md.)
std::shared_ptr<AudioOutput> createAudioOutput(const OutputConfig& cfg);

} // namespace sdrscan
