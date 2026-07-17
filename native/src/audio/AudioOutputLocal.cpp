#include "AudioOutputLocal.h"
#include "../dsp/Const.h"

#include <algorithm>
#include <iostream>

namespace sdrscan {

namespace {
constexpr unsigned long kFramesPerBuffer = 1000;
}

AudioOutputLocal::AudioOutputLocal() {
    if (Pa_Initialize() == paNoError) {
        paInitialized_ = true;
    } else {
        std::cerr << "AudioOutputLocal: PortAudio init failed - local audio disabled\n";
    }
}

AudioOutputLocal::~AudioOutputLocal() {
    close();
    if (paInitialized_) Pa_Terminate();
}

void AudioOutputLocal::reconnect() {
    // Throttle attempts regardless of outcome - if there's genuinely no usable output device
    // (e.g. WSL2 with no host audio), send() would otherwise call this on every mixer tick
    // (~1000x/sec), spamming logs and burning CPU for no benefit.
    nextReconnectAttempt_ = std::chrono::steady_clock::now() + kReconnectCooldown;

    close();
    if (!paInitialized_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    outputBuffer_.clear();

    PaError err = Pa_OpenDefaultStream(&stream_, 0, 1, paInt16, AUDIO_SAMPLERATE, kFramesPerBuffer,
                                        &AudioOutputLocal::paCallback, this);
    if (err != paNoError) {
        std::cerr << "AudioOutputLocal: failed to open stream (retrying in "
                   << kReconnectCooldown.count() << "s): " << Pa_GetErrorText(err) << "\n";
        stream_ = nullptr;
        return;
    }
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        std::cerr << "AudioOutputLocal: failed to start stream (retrying in "
                   << kReconnectCooldown.count() << "s): " << Pa_GetErrorText(err) << "\n";
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
}

void AudioOutputLocal::close() {
    if (stream_) {
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
}

int AudioOutputLocal::paCallback(const void* /*input*/, void* output, unsigned long frameCount,
                                  const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                  PaStreamCallbackFlags /*statusFlags*/, void* userData) {
    auto* self = static_cast<AudioOutputLocal*>(userData);
    auto* out = static_cast<int16_t*>(output);

    std::lock_guard<std::mutex> lock(self->mutex_);
    unsigned long i = 0;
    for (; i < frameCount && !self->outputBuffer_.empty(); i++) {
        out[i] = self->outputBuffer_.front();
        self->outputBuffer_.pop_front();
    }
    for (; i < frameCount; i++) out[i] = 0;

    // If the buffer is growing (consumer running ahead of producer), drop the backlog to
    // avoid building up latency - same policy as AudioServer.py's _pyAudioCb.
    if (self->outputBuffer_.size() > kFramesPerBuffer * 2) {
        size_t toDrop = self->outputBuffer_.size() - kFramesPerBuffer;
        self->outputBuffer_.erase(self->outputBuffer_.begin(), self->outputBuffer_.begin() + toDrop);
    }

    return paContinue;
}

void AudioOutputLocal::send(const std::vector<int16_t>& samples) {
    if (!paInitialized_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        outputBuffer_.insert(outputBuffer_.end(), samples.begin(), samples.end());
        // paCallback() normally trims this, but it never runs while there's no active
        // stream (e.g. during the reconnect cooldown below) - cap it here too so a
        // persistently-unavailable device doesn't grow this without bound.
        if (outputBuffer_.size() > kFramesPerBuffer * 4) {
            size_t toDrop = outputBuffer_.size() - kFramesPerBuffer * 2;
            outputBuffer_.erase(outputBuffer_.begin(), outputBuffer_.begin() + toDrop);
        }
    }

    if ((!stream_ || Pa_IsStreamActive(stream_) != 1) &&
        std::chrono::steady_clock::now() >= nextReconnectAttempt_) {
        reconnect();
    }
}

} // namespace sdrscan
