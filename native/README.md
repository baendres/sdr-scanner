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

### Docker (recommended)

The easiest way to run this - one command pulls in every dependency (GNU Radio, SoapySDR +
the RTL-SDR driver, etc.), no manual `apt-get` list to keep in sync:

```
docker compose up -d --build
```

Notes:

- `--privileged` + `/dev/bus/usb:/dev/bus/usb` (already in `docker-compose.yaml`) gives the
  container USB access for RTL-SDR/SoapySDR hardware.
- `./data` is mounted into the container for the SQLite database, so config (and any live
  changes made through the API) survives `docker compose restart` / container recreation.
- `restart: on-failure` is what makes the settings page's **Restart Now** button (see "The 'no
  restart' mechanism" below) actually bring the process back - it exits with a distinct
  non-zero code and relies on this policy to relaunch it with the new receiver/output config
  loaded. Deliberately *not* `unless-stopped`/`always`: those also auto-start the container
  whenever the host reboots, which is unwanted if you're running this alongside another
  project's containers on the same machine and want the reboot to bring *that* one back, not
  this one (see the note on `docker-compose.yaml`'s `restart` line). Running the bare binary
  directly (the "Local" section below) has no restart policy at all, so the button just stops
  the process there.
- `name: sdr-scanner-native` pins the Compose project name so it doesn't shift if you ever
  rename the checkout directory - Compose otherwise derives the project name (and therefore
  the image/container names) from the directory name, so a rename looks like a brand-new
  project and triggers a full rebuild.

### VOLK kernel tuning (CPU usage)

GNU Radio's DSP blocks (FIR filters, magnitude/RSSI calc, FM demod, etc.) go through VOLK,
which picks a SIMD kernel (AVX2/AVX/SSE/...) per function based on a one-time benchmark against
the actual CPU - `volk_profile`. Without it, VOLK falls back to generic (non-SIMD)
implementations, which measured 3-5x slower on this app's hot-path functions (FM demod, RSSI
magnitude, sample deinterleaving) on real hardware - a meaningful chunk of `sdrscan`'s CPU
usage if it's never been run.

VOLK reads its config from `$HOME/.volk/volk_config` at process startup. The container runs as
root with no `USER` directive (so `$HOME=/root`), which is *not* the same `$HOME` as whatever
user profiles from the host shell - profiling on the host has no effect on the containerized
process. `./volk:/root/.volk` (in `docker-compose.yaml`) persists the container's own profile
across rebuilds; to (re-)generate it:

```
docker compose up -d                          # make sure the mount above exists
docker exec -it <container-name> volk_profile # takes a few minutes; writes into the mount
docker compose restart                        # sdrscan only reads volk_config at startup
```

Re-run this after any change to the deployment host's CPU (a migration to different hardware,
mainly) - a profile tuned for one CPU doesn't necessarily transfer to another.

### Local (fast dev loop)

For iterating on the code itself. Ubuntu/Debian's `gnuradio-dev` package (3.10.x) has
everything needed - no need to build GNU Radio from source:

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

## Migrating from the Python app's `sdrscan.yaml`

```
./build/sdrscan_import_yaml /path/to/sdrscan.yaml /path/to/sdrscan.db
```

One-shot: reads the YAML and seeds a SQLite database with the same channels/receivers/outputs.
After that, the database is the source of truth - edit it through the control API (or with the
`sqlite3` CLI directly if you prefer), not by re-running the import.

## Running

For the local/bare-binary build above (`docker compose up` already starts the container with
the right flags - see "Docker (recommended)"):

```
./build/sdrscan -d sdrscan.db -w web --host 0.0.0.0 --port 8080
```

Then open `http://<host>:8080/`. A kiosk-style control page for a small touchscreen (built and
verified against a 480x320 landscape panel, e.g. a 3.5" RPi HDMI touchscreen) is at
`http://<host>:8080/panel_ui/index.html` - reuses `app.js` unmodified (its DOM IDs match what
`app.js` already expects, including the squelch/CTCSS/gain fields, which are just relocated -
see below), with its own `panel.css`/`panel.js` for the layout:
- A compact **live active-channels list** (only channels seen active recently, same data
  app.js's `renderActiveList()` always drove) for normal at-a-glance scanning, next to a 2x3
  grid of Hold/Solo/Mute/Force/Enable/Disable buttons for whichever channel is selected.
- Two slide-in flyouts (`panel.js`), reached via buttons on the dock, for the case the live
  list can't cover - reaching a specific channel to configure it:
  - **Channels** - every configured channel, active or not (a disabled or
    simply-never-triggered one still needs to be reachable), tap one to select it and return
    to the dock.
  - **Config** - squelch/CTCSS/adaptive-margin/gain for the now-selected channel; tuned rarely
    enough that it doesn't need to compete with the action buttons for space.

No server-side route registration needed - `HttpServer`'s static handler serves any path under
`web/` generically, so a new page here is just a new file.

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
  fans out to the configured `AudioOutput`s. Ticks a heartbeat every loop iteration;
  `Scanner`'s maintenance loop watches it (`AudioMixer::isAlive()`) and, if it ever goes stale
  (the thread hung or returned without an exception - port of Scanner.py's
  `audioServerProcess.is_alive()` watchdog, adapted since this is a thread sharing the process
  rather than a separate OS process), logs it and exits non-zero so the deployment's restart
  policy recovers a silently-dead audio pipeline instead of leaving it stuck with everything
  else (receivers, HTTP server) still running.
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

- **Hot** (no interruption): squelch threshold, adaptive squelch margin, CTCSS tone, audio
  gain, dwell time, mute/solo/hold/enabled/forceActive.
- **Structural** (brief in-process rebuild on the affected receiver(s), still zero container
  restart): add/remove a channel, change frequency, change `maxChannelsPerWindow`. This calls
  `Scanner::buildWindows()`, which mirrors the Python version's window-building algorithm but
  can now be triggered live instead of only at startup.
- **Database-only** (receivers, audio outputs): these are read once at startup, so edits take
  effect on the next process restart, not live. The settings page shows a "restart needed"
  banner with a **Restart Now** button (`POST /api/restart`) once you've made one of these
  changes - it requests a graceful shutdown (the same path SIGTERM/Ctrl+C use) but exits with a
  distinct non-zero code, and relies on the deployment's restart policy (docker-compose's
  `restart: on-failure`, see `native/docker-compose.yaml`) to bring the process back up with
  the new config loaded. Running the binary directly with no restart policy means that button
  just stops it. To make a receiver easier to find in the first place, `GET
  /api/receivers/scan` enumerates connected SDR hardware for the settings page's **Scan for
  Receivers** button - and starting the process with zero receivers configured (e.g. a brand
  new database) is itself a valid state now, specifically so the web UI comes up far enough to
  use that button on a first-time setup.

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

### Adaptive squelch and squelch debounce

Two independent additions to the squelch path, both aimed at handling noisier band conditions
than a single fixed threshold tolerates well:

- **Adaptive ("noise-relative") squelch** - a channel's `squelchNoiseMargin_dB` config field
  (unset by default) overrides `squelchThreshold`: when set, the effective threshold tracks the
  channel's own live noise floor estimate (`noiseFloor_dBFS_`, already computed for telemetry -
  see `updateRSSI()`) plus this margin, instead of a fixed dB value that needs to be re-tuned by
  hand as interference or time-of-day conditions change. `ChannelBlockBase::
  effectiveSquelchThreshold()` resolves which one actually applies; it falls back to the fixed
  `squelchThreshold` until a noise floor estimate exists yet (nothing to be adaptive relative to)
  or whenever no margin is configured. Setting an explicit `squelchThreshold` via the API clears
  the margin and vice versa in spirit (the margin isn't cleared by a squelch write, but
  `effectiveSquelchThreshold()` treats them as alternate modes, not additive) - only one is ever
  actually in effect at a time. `ChannelBlockEAS` doesn't have a squelch block of its own; it
  just forwards the margin to its internal `ChannelBlockFM`. The resolved threshold is only ever
  pushed to the real squelch block (`set_threshold()`) from `getStatus()`/
  `refreshAdaptiveSquelchThreshold()` - i.e. from Scanner's control-plane thread, the same one
  every other hot-update setter in this codebase already runs on - never from `updateRSSI()`
  itself, which runs on the flowgraph's own worker thread. An earlier version pushed it straight
  from `updateRSSI()`, calling one live block's setter from inside a different block's `work()`
  call on the flowgraph thread - a threading pattern found nowhere else in this codebase, and one
  that hung the flowgraph's scheduler on real hardware (observed as `AudioMixer: receiver N
  starved for ... samples (~100% silence-filled)` and the liveness watchdog eventually firing) as
  soon as a channel had an adaptive margin configured. `ChannelBlockEAS` calls
  `refreshAdaptiveSquelchThreshold()` explicitly on its internal `ChannelBlockFM` for this reason
  too - it never calls that block's own `getStatus()` (see the class comment), so without this
  explicit nudge a NOAA/BFM_EAS channel's adaptive threshold would never update at all.
- **Squelch debounce** - `ChannelBlockBase::debounceSquelch()` requires the raw (power/CTCSS)
  squelch-open decision to persist for `SQUELCH_DEBOUNCE_SECONDS` (50ms, `Const.h`) before it's
  treated as a genuine transition, filtering brief noise spikes that would otherwise pop the
  audio gate open and shut. It drives the *real* audio gate (a `mute_ff` block downstream of the
  demod, e.g. `ChannelBlockFM::blockAudioGate_`), not just the reported status - so a spike that
  doesn't persist never reaches a listener as an audible pop, it isn't just hidden from the UI.
  This is a fixed DSP constant rather than a per-channel setting - closer to a real analog
  squelch circuit's hang time than something that needs per-channel tuning. It's wall-clock
  based (`nowUnixSeconds()`), matching how `Scanner` actually polls `getStatus()` every ~100ms in
  production; a `forceActive` operator override bypasses it entirely (an explicit "make this
  audible now" action shouldn't wait out a squelch hang time).

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

### Hop settle time (tunePause) vs. RTL-SDR buffer depth

`SoapyReceiver` never stops the flowgraph between windows - it redirects the RF selector to a
window's chain and, after a short settle delay, starts trusting what comes out (see the class
comment on `SoapyReceiver.h`). That delay exists because samples from the *old* window are
still sitting in the async read queue at hop time; too short a delay relative to that queue's
depth means the new window's squelch/demod briefly process old-frequency samples - upstream
`sdr-scanner`'s README calls this out by name ("Tuning-Pause Settings") as a cause of "invalid
squelch breaks", audible as spurious noise/false triggers right after a hop.

Upstream's calibrated default pairs `buffers=8` with a 20ms pause. This receiver type uses
`buffers=32` instead (4x deeper - see the `streamArgs` comment in `SoapyReceiver.cpp` for why:
it's there to survive USB completion latency on a virtualized passthrough, e.g. WSL2's
usbipd), which needs a correspondingly longer settle time - `kRtlSdrTuneSettleMs` (80ms,
scaled roughly with the buffer depth) exists for exactly that reason. If a specific device
still shows hop-boundary noise/clicks, that value is the first thing to try raising; it isn't
yet exposed as a per-receiver config knob the way upstream's `tunePause` is, since no hardware
has needed that granularity yet.

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
- `PATCH /api/channels/{id}` - body may include any of: `squelchThreshold`,
  `squelchNoiseMargin_dB` (number or `null` to go back to a fixed `squelchThreshold` - see
  "Adaptive squelch" above), `ctcssToneHz` (number or `null` to disable), `audioGain_dB`,
  `dwellTime_s`, `mute`, `solo` (`true`/`false`/`null`), `hold`, `forceActive`, `enabled`,
  `disableUntil` (unix seconds).
- `POST /api/channels` - body: `{freq_hz, label?, mode?, audioGain_dB?, dwellTime_s?,
  squelchThreshold?, squelchNoiseMargin_dB?, ctcssToneHz?}` -> `{id}`.
- `DELETE /api/channels/{id}`
- `PATCH /api/scanner` - body: `{maxChannelsPerWindow}`
- `GET /api/receivers/scan` - enumerates connected SDR hardware (`SoapySDR::Device::enumerate()`
  across every installed driver module) -> `{devices: [{driver, label, serial, args}]}`. Used
  by the settings page's **Scan for Receivers** button; safe to call with nothing plugged in
  (returns `{devices: []}`).
- `POST /api/restart` - the settings page's **Restart Now** button (see "Database-only" above).

`GET /ws` (WebSocket): sends a `Snapshot` on connect, then broadcasts `ChannelConfig` /
`ChannelStatus` / `ScanWindowStart` / `ScanWindowDone` / `ScanWindowConfigsChanged` messages.
Accepts the same control messages as the REST PATCH fields, as `{"type": "...", "data": {...}}`
- e.g. `ChannelMute`, `ChannelHold`, `ChannelSolo`, `ChannelEnable`, `ChannelDisableUntil`,
`ChannelForceActive`, `ChannelSetSquelch`, `ChannelSetSquelchNoiseMargin`, `ChannelSetCtcss`,
`ChannelSetAudioGain`, `ChannelSetDwellTime`.

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
build small synthetic flowgraphs (`gr::analog::sig_source_c` / `frequency_modulator_fc` /
`vector_source_c` standing in for a receiver) feeding directly into
`ChannelBlockFM`/`ChannelBlockAM`/`ChannelBlockEAS`, and assert on squelch/CTCSS/tone-detect
open-closed behavior - this is what caught the CTCSS bug described above.
`tests/test_squelch_debounce.cpp` covers the adaptive squelch and debounce logic (see above)
directly against `ChannelBlockBase` through a trivial concrete subclass, rather than through a
flowgraph - `debounceSquelch()` is wall-clock based, and a flowgraph's `tb->run()` processes
samples as fast as the CPU allows rather than in real time, so there'd be no reliable way to
land inside or outside the debounce window from flowgraph sample counts alone. `test_database.cpp`
round-trips config through SQLite, including reopening the database to prove settings survive
a process restart, and migrating a pre-existing database created before a column existed (the
same situation an already-deployed instance is in after a schema change like this one).
Actually receiving RF and playing audio needs real (or SoapyRemote) SDR hardware, which isn't
available in a CI/dev-container sandbox - verify that part on your own receiver.
