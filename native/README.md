# sdr-scanner (native)

A ground-up rewrite of `sdr-scanner` in C++ using GNU Radio's native API, living
side-by-side with the original Python app (`../sdr_scanner/`) while this matures. The Python
version keeps working unmodified.

## Why this exists

The Python app configures itself once from a YAML file at startup and needs a container
restart for any change (squelch value, adding a channel, etc). This rewrite:

1. Stores all configuration (channels, receivers, outputs, scanner settings) in a **SQLite**
   database instead of YAML.
2. Applies changes to a **running** instance with **no container restart** - a REST/WebSocket
   control API updates the database and the live GNU Radio blocks in the same call.
3. Adds **CTCSS squelch**.
4. Runs as a single multi-threaded process instead of one OS process per receiver +
   multiprocessing shared-memory audio, which the Python version needed only because of the
   process-per-receiver design.

DMR/P25 digital scanning was explicitly out of scope for this pass (see "Explicitly
Deferred" below).

## Status: foundation phase

This is a working foundation, not full feature parity with the Python app yet:

- **Implemented**: SQLite config + live control API, RTL-SDR/Soapy receivers, FM/NFM/AM/
  NOAA/BFM_EAS demod, CTCSS squelch, multi-receiver scan-window scheduling, audio mixing with
  Local (PortAudio) / UDP / WebSocket / Icecast outputs, the web UI (including a settings page
  for editing channels/receivers/outputs).
- **Deferred** (see below): SSB, DMR/P25.

## Building

### Local (fast dev loop)

Ubuntu/Debian's `gnuradio-dev` package (3.10.x) has everything needed - no need to build
GNU Radio from source:

```
sudo apt-get install build-essential cmake pkg-config gnuradio-dev \
    libboost-dev libsqlite3-dev nlohmann-json3-dev portaudio19-dev \
    libsoapysdr-dev soapysdr-module-all soapysdr-module-rtlsdr rtl-sdr librtlsdr-dev \
    libyaml-cpp-dev libmp3lame-dev catch2

cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j$(nproc)
```

Run the test suite (synthetic-signal DSP tests + a database round-trip test, no SDR hardware
needed):

```
./build/tests/sdrscan_tests
```

### Docker

```
docker compose build
docker compose up
```

The container mounts `./data` for the SQLite database, so config (and any live changes made
through the API) survives `docker compose restart` / container recreation.

## Migrating from the Python app's `sdrscan.yaml`

```
./build/sdrscan_import_yaml /path/to/sdrscan.yaml /path/to/sdrscan.db
```

One-shot: reads the YAML and seeds a SQLite database with the same channels/receivers/outputs.
After that, the database is the source of truth - edit it through the control API (or with the
`sqlite3` CLI directly if you prefer), not by re-running the import.

## Running

```
./build/sdrscan -d sdrscan.db -w web --host 0.0.0.0 --port 8080
```

Then open `http://<host>:8080/`.

## Architecture

Single process, multiple threads (see also the comment block at the top of
`src/scanner/Scanner.h` and `src/receiver/SoapyReceiver.h`):

- **Main thread**: loads config from SQLite, runs `Scanner`'s maintenance loop (re-enables
  channels whose temporary disable has expired).
- **One thread per receiver** (`SoapyReceiver::run`): owns that receiver's `gr::top_block`,
  which has *every* configured `ScanWindow` wired into it at once (source -> RF `selector` ->
  each window's demod chain -> audio `selector` -> the audio sink). The thread round-robins
  between windows by flipping both selectors' live index - no flowgraph stop/restart, no USB
  re-negotiation on the hardware source. Only a structural change to the window set itself
  (add/remove/edit a channel or receiver) tears down and rebuilds this flowgraph.
- **One audio mixer thread** (`AudioMixer::run`): drains each receiver's ring buffer, mixes,
  fans out to the configured `AudioOutput`s.
- **HTTP server threads** (`HttpServer`, Boost.Beast, thread-per-connection): REST API,
  WebSocket control/status channel, and the static web UI.

### The "no restart" mechanism

Every mutating REST/WS request:

```
Client -> HttpServer -> Scanner::setChannelX(...) -> write-through to SQLite
                                                    -> Scanner::updateChannel() looks up the
                                                       live block on every receiver
                                                    -> block->setX(...)   (GNU Radio blocks
                                                       support live parameter changes while
                                                       running - the same mechanism GRC's
                                                       live GUI widgets use)
```

Two tiers:

- **Hot** (no interruption): squelch threshold, CTCSS tone, audio gain, dwell time,
  mute/solo/hold/enabled/forceActive.
- **Structural** (brief in-process rebuild on the affected receiver(s), still zero container
  restart): add/remove a channel, change frequency, change `maxChannelsPerWindow`. This calls
  `Scanner::buildWindows()`, which mirrors the Python version's window-building algorithm but
  can now be triggered live instead of only at startup.
- **Database-only** (receivers, audio outputs): these are read once at startup, so edits take
  effect on the next process restart, not live. The settings page shows a "restart needed"
  banner with a **Restart Now** button (`POST /api/restart`) once you've made one of these
  changes - it requests a graceful shutdown (the same path SIGTERM/Ctrl+C use) and relies on
  the deployment's restart policy (docker-compose's `restart: unless-stopped`, see
  `native/docker-compose.yaml`) to bring the process back up with the new config loaded.
  Running the binary directly with no restart policy means that button just stops it.

### CTCSS squelch

`ChannelBlockFM` always wires in an `analog::ctcss_squelch_ff` after the FM demodulator (so no
flowgraph topology change is ever needed to turn it on/off). A channel only reports "active"
when the power squelch is open **and**, if a CTCSS tone is configured, that tone is present.

Implementation note / pitfall we hit and fixed: the first version tried to make "CTCSS
disabled" a no-op by setting the block's required signal level to 0 (on the theory that any
nonzero energy would trivially exceed a 0 threshold). That's unreliable - a signal with
literally no energy in the narrow CTCSS analysis band (an unmodulated test carrier, or a
genuinely quiet moment) reads `unmuted() == false` even at level 0, because the internal
comparison is strict (`energy > level`). The fix (see `ChannelBlockFM::getStatus()`) is to
simply not consult the CTCSS block's `unmuted()` at all unless a tone is actually configured
(or the channel is force-active), rather than relying on the block to self-disable.

### NOAA / BFM_EAS (attention-tone) channel modes

`ChannelBlockEAS` wraps an internal `ChannelBlockFM` (the demod + RF power squelch) and taps
its demodulated audio with an FFT tone detector (`EasToneDetectBlock`) looking for a
configured alert tone: NOAA weather radio's 1050 Hz SAME tone, or Broadcast EAS's 853/960 Hz
two-tone signal. Unlike the squelch-driven modes, "active" is a debounced latch - 3 consecutive
tone-detect triggers open the audio gate and start a `dwellTime_s` countdown, so a channel
stays audible/`ACTIVE` for the rest of the alert even if the tone detector misses a frame or
two between bursts.

BFM_EAS relies on `ChannelBlockFM`'s wideband ("quad rate") support: when a channel's
deviation exceeds the audio sample rate, demodulation runs at a higher internal rate (a
multiple of the audio rate, wide enough to cover a ~200kHz WBFM station) and the audio filter
decimates back down in the same step it applies the audio bandpass - narrowband channels are
unaffected.

### Simplifications vs. the Python version (documented in code comments too)

- Input channelization always uses a single-stage `freq_xlating_fir_filter_ccf` rather than
  the Python version's optional two-stage FFT-filter split for very high decimation ratios.
  That was a CPU optimization, not a correctness requirement - revisit if profiling shows
  it's needed for a particular receiver's sample rate.
- FM de-emphasis is implemented directly via `iir_filter_ffd` with hand-computed coefficients
  (the standard GNU Radio de-emphasis formula), since `fm_deemph`/`nbfm_rx` are GRC-only
  hierarchical blocks with no C++ class - same for the input channelization decimation.
- `ChannelBlockEAS`'s tone detector does its own windowing/FFT/magnitude instead of using
  `fft.logpwrfft_f` (also GRC/Python-only, no C++ class), and compares tone-vs-reference-band
  energy directly in linear power rather than converting to dB first - mathematically
  equivalent to Python's dB-domain threshold comparison, just skips a conversion step.

## REST / WebSocket API

- `GET /api/state` - full snapshot (channels, receivers, outputs, scanner settings, live
  channel statuses).
- `PATCH /api/channels/{id}` - body may include any of: `squelchThreshold`, `ctcssToneHz`
  (number or `null` to disable), `audioGain_dB`, `dwellTime_s`, `mute`, `solo` (`true`/`false`/
  `null`), `hold`, `forceActive`, `enabled`, `disableUntil` (unix seconds).
- `POST /api/channels` - body: `{freq_hz, label?, mode?, audioGain_dB?, dwellTime_s?,
  squelchThreshold?, ctcssToneHz?}` -> `{id}`.
- `DELETE /api/channels/{id}`
- `PATCH /api/scanner` - body: `{maxChannelsPerWindow}`

`GET /ws` (WebSocket): sends a `Snapshot` on connect, then broadcasts `ChannelConfig` /
`ChannelStatus` / `ScanWindowStart` / `ScanWindowDone` / `ScanWindowConfigsChanged` messages.
Accepts the same control messages as the REST PATCH fields, as `{"type": "...", "data": {...}}`
- e.g. `ChannelMute`, `ChannelHold`, `ChannelSolo`, `ChannelEnable`, `ChannelDisableUntil`,
`ChannelForceActive`, `ChannelSetSquelch`, `ChannelSetCtcss`, `ChannelSetAudioGain`,
`ChannelSetDwellTime`.

Note: the Python web UI had grown a PIN-based "listen only vs. control" access gate
(`SDRSCANNER_CONTROL_PIN`). That's not reimplemented here yet - every connected client can
control the scanner. If you need that back, it'd be a reasonable addition to `HttpServer`, or
handle it at a reverse-proxy layer in front of this.

## Explicitly deferred

- **SSB channel mode** - same `ChannelBlockBase` extension point as
  `ChannelBlockFM`/`ChannelBlockAM`/`ChannelBlockEAS`; a straightforward follow-up.
- **wxPython GUI** - dropped in favor of the web UI (the Python repo's own README already
  listed this as a TODO).
- **DMR/P25** - per the original request, deferred entirely. If tackled later, realistically
  means shelling out to an existing decoder (OP25/DSDcc) rather than reimplementing AMBE.
- **Per-client control PIN/role gate** - see note above.

## Testing without SDR hardware

`tests/test_channel_fm.cpp`, `tests/test_ctcss_squelch.cpp`, and `tests/test_channel_eas.cpp`
build small synthetic flowgraphs (`gr::analog::sig_source_c` / `frequency_modulator_fc`
standing in for a receiver) feeding directly into `ChannelBlockFM`/`ChannelBlockAM`/
`ChannelBlockEAS`, and assert on squelch/CTCSS/tone-detect open-closed behavior - this is what
caught the CTCSS bug described above. `test_database.cpp`
round-trips config through SQLite, including reopening the database to prove settings survive
a process restart. Actually receiving RF and playing audio needs real (or SoapyRemote) SDR
hardware, which isn't available in a CI/dev-container sandbox - verify that part on your own
receiver.
