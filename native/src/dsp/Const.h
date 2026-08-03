#pragma once

namespace sdrscan {

constexpr int AUDIO_SAMPLERATE = 16000;

// Set a maximum samplerate to limit processing requirements.
constexpr int MAX_RF_SAMPLERATE = 2'500'000;

// Squelch averaging filter time constant.
constexpr double SQUELCH_TC = 0.0125;

// How long the raw squelch-open/closed decision must persist before it's treated as a genuine
// transition (drives both the reported status and the actual audio gate - see
// ChannelBlockBase::debounceSquelch()). Filters brief noise spikes from opening/closing the
// squelch on their own; not user-configurable, this is closer to a real analog squelch
// circuit's hang time than a per-channel tuning knob.
constexpr double SQUELCH_DEBOUNCE_SECONDS = 0.05;

// RSSI LowPass RC filter alpha.
constexpr double RSSI_LOWPASS_TC = 0.125;
constexpr double RSSI_UPDATE_FREQ_HZ = 4.0;
constexpr double STATUS_UPDATE_TIME_S = 1.0 / RSSI_UPDATE_FREQ_HZ;

// Noise floor RC filter alpha.
constexpr double NOISEFLOOR_LOWPASS_TC = 60.0;
constexpr double NOISEFLOOR_LOWPASS_A = 1.0 / (RSSI_UPDATE_FREQ_HZ * NOISEFLOOR_LOWPASS_TC);

// Volume RC filter alpha (separate attack/decay).
constexpr double VOLUME_LOWPASS_ATTACK_TC = 0.025;
constexpr double VOLUME_LOWPASS_DECAY_TC = 0.25;

// Default CTCSS tone detector required signal level (see analog::ctcss_squelch_ff).
constexpr float CTCSS_DEFAULT_LEVEL = 0.01f;

// Arbitrary floor used when a magnitude is ~0 (avoids log10(0)).
constexpr float DBFS_FLOOR = -150.0f;

// DMR channels need an RF sample rate that's a whole multiple of this (see ChannelBlockDMR.cpp's
// kDiscriminatorRate) - ScanWindow::selectRfSampleRate() needs to know this constraint too, to
// avoid picking a rate that would make ChannelBlockDMR's constructor throw.
constexpr int DMR_DISCRIMINATOR_RATE_HZ = 48000;

} // namespace sdrscan
