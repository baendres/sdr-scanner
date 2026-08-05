Testing ability to edit!

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

DMR/P25 digital scanning was explicitly out of scope for the initial foundation pass; DMR
(conventional) landed since then - see "DMR/P25 digital decode" below. P25 (conventional and
trunked) is still deferred (see "Explicitly Deferred").

## Status: foundation phase

This is a working foundation, not full feature parity with the Python app yet:

- **Implemented**: SQLite config + live control API, RTL-SDR/Soapy receivers, FM/NFM/AM/
  NOAA/BFM_EAS/DMR (conventional) demod, CTCSS squelch, multi-receiver scan-window scheduling,
  audio mixing with Local (PortAudio) / UDP / WebSocket / Icecast outputs, the web UI (including
  a settings page for editing channels/receivers/outputs).
- **Deferred** (see below): SSB, P25 (conventional and trunked).

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
```

mbelib + DSDcc (DMR/P25 AMBE/IMBE decode - see "DMR/P25 digital decode" below) aren't in apt;
build+install them from source once, same steps `Dockerfile` runs:

```
git clone --depth 1 https://github.com/f4exb/mbelib.git /tmp/mbelib
cmake -S /tmp/mbelib -B /tmp/mbelib/build -DCMAKE_BUILD_TYPE=Release -DDISABLE_TEST=ON
cmake --build /tmp/mbelib/build -j$(nproc) && sudo cmake --install /tmp/mbelib/build

git clone --depth 1 https://github.com/f4exb/dsdcc.git /tmp/dsdcc
cmake -S /tmp/dsdcc -B /tmp/dsdcc/build -DCMAKE_BUILD_TYPE=Release -DUSE_MBELIB=ON -DBUILD_TOOL=OFF
cmake --build /tmp/dsdcc/build -j$(nproc) && sudo cmake --install /tmp/dsdcc/build
sudo ldconfig
```

Then:

```
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
verified against a landscape SPI touchscreen, e.g. a "3.5-inch RPi touchscreen" using the mhs35
overlay) is at `http://<host>:8080/panel_ui/index.html` - sized for a **720x480 CSS pixel
viewport**, not the panel's own advertised 480x320: with `display_auto_detect=0` and
`hdmi_ignore_hotplug=1` set (a common config for these SPI panels, since there's no real
HDMI/EDID negotiation to do), Chromium was found on real hardware to be rendering into a separate
720x480 canvas that gets mirrored/scaled down onto the physical 480x320 panel (`fbcp`-style)
rather than talking to it directly - confirmed with `window.innerWidth`/`innerHeight` from inside
the page itself (see `panel.js`'s git history), since `xrandr`'s own reported mode name
(`Composite-1`, `0mm x 0mm`, `FIXED_MODE`) turned out to be an unreliable way to infer this from
outside the browser. If you're deploying to a *different* panel, check
`window.innerWidth`/`innerHeight` the same way rather than assuming a resolution from the
hardware's spec sheet - `panel.css`'s sizes only look right at the true CSS viewport, not
whatever the panel driver advertises to X11. Reuses `app.js` (its DOM IDs match what `app.js`
already expects, including the squelch/CTCSS/gain fields, which are just relocated - see below),
with its own `panel.css`/`panel.js` for the layout:
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
- **Audio starts itself and shows whether it's actually playing.** A kiosk has no keyboard/mouse
  to find and click a "Start" button with, so the page sets `data-autostart-audio` on `<body>`
  (see `app.js`'s `init()`) to call the same `startAudio()` the main page's button does, on load.
  Browsers still generally require a user gesture before they'll actually produce audio output
  (autoplay policy) - if that blocks it, the AudioContext just comes up suspended rather than
  failing outright, and a document-wide click listener resumes it on the panel's very first tap
  (any tap, not just a specific button - the whole kiosk screen is "gesture enough"). Either way,
  a colored dot plus status text in its own `.audio-bar` (`data-external-audio-controls` on
  `<body>` opts out of `app.js`'s default header-injected controls, which have no room on this
  screen size) reports the real state - `app.js`'s `currentAudioState()`/`refreshAudioIndicator()`
  drive it from actual observable signals (AudioContext running, a PCM frame received recently)
  rather than "did we call connect," so e.g. a connected-but-silent upstream shows red ("no
  audio") the same as a fully dropped connection would, not a falsely reassuring "connected."
  The main page uses the same state machine for its own (header) `#audioStatus` text, unchanged.
- **Recovers from a WebSocket stuck in `CONNECTING` forever.** Found on a real phone: turning
  WiFi off mid-connection left the audio WebSocket in `CONNECTING` indefinitely - no `close`/
  `error` event ever fired on their own, so the normal `onclose`-based retry loop (`app.js`'s
  `connectAudioWs()`/`connectWS()`) never got a chance to run, and the UI just sat on
  "connecting..." with no way to recover short of a manual reload. `armStuckConnectTimeout()`
  force-closes a handshake that hasn't resolved within 6s (calling `close()` on a `CONNECTING`
  socket reliably fires `close` per spec, which *does* hand it back to the retry loop), and
  `online`/`visibilitychange` listeners retry immediately once the browser reports the network
  came back or the tab became active again, rather than waiting out that fixed delay.

No server-side route registration needed - `HttpServer`'s static handler serves any path under
`web/` generically, so a new page here is just a new file.

### Authentication (off by default)

The control API (static files, REST, and the WS upgrade) has **no authentication unless you turn
it on** - fine for the trusted-home-LAN deployment this was built for, but not if you port-forward
`8080`/`8123` for remote access (e.g. to listen in away from home) rather than something like a
WireGuard/VPN tunnel back into the LAN. To require HTTP Basic Auth, set both
`SDRSCAN_AUTH_USER`/`SDRSCAN_AUTH_PASSWORD` (`docker-compose.yaml`'s `environment:` section, or
directly in the shell for the bare binary) before starting the container - unset (the default)
means auth stays off. This is a basic gate (not constant-time, no rate limiting, no per-user
accounts) meant to keep the control API from being wide open on a forwarded port, not a substitute
for a real VPN if you want it properly secured.

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
  `refreshAdaptiveSquelchThreshold()` - a separate thread from the flowgraph's own worker thread,
  the same one every other hot-update setter in this codebase already runs on - never from
  `updateRSSI()` itself, which runs on the flowgraph's own worker thread (called from
  `Mag2ToPowerBlock`'s `work()`). An earlier version pushed it straight from `updateRSSI()`,
  calling one live block's setter from inside a different block's `work()` call on the flowgraph
  thread - a threading pattern found nowhere else in this codebase, and one that hung the
  flowgraph's scheduler on real hardware (observed as `AudioMixer: receiver N starved for ...
  samples (~100% silence-filled)` and the liveness watchdog eventually firing) as soon as a
  channel had an adaptive margin configured. `ChannelBlockEAS` calls
  `refreshAdaptiveSquelchThreshold()` explicitly on its internal `ChannelBlockFM` for this reason
  too - it never calls that block's own `getStatus()` (see the class comment), so without this
  explicit nudge a NOAA/BFM_EAS channel's adaptive threshold would never update at all.

  `getStatus()` itself turned out to run far more often than assumed when the above fix landed -
  `SoapyReceiver::checkCurrentWindow()` calls it for every channel in the active window on every
  iteration of its ~1ms window-scheduling loop (see `SoapyReceiver::run()`), not the ~100ms
  cadence of Scanner's REST/WS-triggered control-plane updates. That's roughly 1000x more often
  than the underlying noise floor estimate can actually change (`RSSI_UPDATE_FREQ_HZ`, 4Hz), and
  calling into a live GNU Radio block's `set_threshold()` that often - once per adaptive-squelch
  channel in the window - was itself enough overhead on real hardware to make a receiver's audio
  production chronically fall behind real time (`~40-70% silence-filled`, not the ~100% of a full
  hang, but still enough to eventually trip the liveness watchdog). `ChannelBlockBase::
  adaptiveThresholdChanged()` fixes this by only pushing when the computed threshold has actually
  moved since the last push, which in practice throttles it back down to the noise floor's own
  update rate.

  That fix alone didn't fully resolve the chronic falling-behind symptom on real hardware - a
  second instance of the exact same overhead pattern was found afterward via per-thread CPU
  profiling (`top -H` inside the container): a single, unnamed `sdrscan` thread (not one of GNU
  Radio's own per-block scheduler threads, which show up named after their block type) was
  pinned near 100% of one core, while the rest of the system had headroom - i.e. not a
  system-wide CPU or USB-bandwidth ceiling, but one specific hot thread. That thread is
  `SoapyReceiver::run()`'s ~1ms window-scheduling loop, the same one driving `getStatus()`'s
  call frequency above. `getStatus()` was also calling the inline audio gate's
  `set_mute()` (`ChannelBlockFM`/`ChannelBlockAM::blockAudioGate_`) unconditionally on every
  call, regardless of whether the debounced mute decision had actually changed -
  `ChannelBlockBase::setAudioGateMuted()` applies the same "only push on genuine change" guard
  used for the adaptive threshold.

  That fix *also* didn't move the chronic starvation numbers on real hardware, which prompted
  actually measuring the hop-hopping cost directly instead of continuing to guess: temporary
  diagnostic logging (hops/sec and cumulative `kRtlSdrTuneSettleMs` overhead per receiver,
  reported alongside `AudioMixer`'s own starvation log) showed hop settle time accounting for a
  fairly steady ~16-24% of wall time - a real cost, but well short of the ~30-68% (averaging
  around 45%) starvation actually observed, and the two didn't track each other: the same
  measured settle overhead (e.g. "5 hops, 20%") showed up next to wildly different starvation
  percentages from one second to the next. That pointed back to the ~1ms loop interval itself
  (not any specific redundant call within it, both of which were already fixed above) as the
  remaining cost: at that rate, `checkCurrentWindow()`/`getStatus()` acquires each live block's
  parameter lock (`unmuted()` reads, etc.) roughly 1000 times/sec per channel in the active
  window, and lock acquisition has real overhead even for a read. Nothing downstream needs that
  granularity - window-hop timing works in ~100ms-1s increments, squelch debounce is 50ms, RSSI
  updates at 4Hz - so `SoapyReceiver::run()`'s loop interval was widened from 1ms to 10ms,
  cutting the polling-driven overhead by ~10x directly rather than continuing to whack-a-mole
  individual calls inside each iteration.

  That fix *still* didn't move the starvation numbers, and a fresh real-hardware capture with
  the hop-rate diagnostic running alongside `AudioMixer`'s log settled it: starvation was
  showing up in seconds with **zero hops** at all, ruling out hop timing as the driver
  entirely - this was happening while a receiver just sat parked on one window, not during
  transitions between windows. That redirected the investigation to the actual DSP compute
  cost of the currently-selected window's demod chain, specifically `ChannelBlockFM`/
  `ChannelBlockAM`'s input channelization filter (`freq_xlating_fir_filter_ccf`) - the same
  block whose `low_pass_2` design was deliberately tightened earlier (see the CTCSS/spur note)
  to reject a hardware spur, at the cost of a much higher tap count than the original `low_pass`
  default. A temporary diagnostic logging that filter's actual tap count at construction time
  confirmed it: **3723-5417 taps**, run continuously at `rfSampleRate` (up to 2.048 Msps) - on
  the order of 10 billion multiply-accumulates/sec for a single active channel, a completely
  different scale of cost than any of the call-overhead or timing issues fixed above, and
  exactly the gap this file's own "Simplifications vs. the Python version" section already
  flagged and deferred (see below) before real-hardware load ever demanded it.

  The fix is **two-stage channelization** (`ChannelBlockBase::splitDecimation()`, used by both
  `ChannelBlockFM` and `ChannelBlockAM`): rather than one filter doing the full sharp selectivity
  (a narrow transition band relative to the RF rate - what actually drives the huge tap count)
  directly at the full RF rate, stage 1 (`freq_xlating_fir_filter_ccf`, still doing the
  frequency translation) uses a *wide* transition band - cheap despite running at the full RF
  rate, and still the same 80dB stopband, since attenuation is what matters for rejecting the
  spur, not transition narrowness - to coarsely decimate down to an intermediate rate. Stage 2
  (a plain `fir_filter_ccf`, no frequency translation needed since stage 1 already did that)
  then applies the *exact same* sharp selectivity as before (identical passband/transition/
  attenuation), just evaluated at that much lower intermediate rate, where the same absolute-Hz
  transition width is a far larger fraction of the sample rate and needs far fewer taps.
  `splitDecimation()` picks the split (smallest stage-2 decimation factor that still evenly
  divides the total, so stage 1 absorbs as much of the reduction as possible) and falls back to
  the original single-stage design when the ratio is too small to be worth splitting - in which
  case `blockChannelFilter_` stays null and behavior is unchanged. Squelch and RSSI both read
  from whichever stage actually produced the final channelized signal, so this is purely a
  compute-cost change, not a filtering/selectivity change - same audio and squelch behavior,
  far less CPU per sample.

  Real production logs confirmed the tap-count reduction (3723-5417 taps down to roughly
  280-480 combined across both stages), but the starvation percentage on real hardware still
  didn't move - the same specific values recurred across this fix and all four before it. With
  every receiver-side overhead and compute hypothesis directly measured and ruled out (and every
  `AudioOutput` implementation checked and confirmed non-blocking - `send()` on all four just
  copies into a mutex-guarded buffer, with actual I/O on a separate thread or callback), the
  remaining candidate was `AudioMixer`'s own pacing assumption rather than anything upstream:
  `kTargetLatencySeconds` (see `AudioMixer::run()`) is the only margin given to GNU Radio's
  inherently bursty block scheduling before a gap counts as "starved," and 100ms may simply not
  be enough room for this pipeline's actual burst interval - which would produce exactly this
  symptom (a persistent, roughly fixed-magnitude deficit unaffected by throughput
  improvements, since the issue isn't that samples arrive too slowly overall, it's that they
  arrive in chunks larger than the assumed margin allows for). Widened to 300ms, with
  `kMixerBufferTargetLen` (the backlog-trim cap) widened alongside it to 500ms so it doesn't
  fight the wider margin by trimming away the exact slack just added.

  That fix *still* didn't move the numbers, which meant the earlier per-thread CPU profiling
  (the one pinned core, back in the `adaptiveThresholdChanged()`/`setAudioGateMuted()`
  investigation above) needed a second, harder look - the assumption that the pinned thread was
  `SoapyReceiver::run()`'s polling loop was never actually proven, just plausible at the time,
  and widening that loop 10x with zero effect on either CPU or starvation is real evidence
  against it. A fresh `top -H` capture settled it by looking at the pinned thread's PR/NI
  columns instead of just its name: `NI -5` is a direct match for `AudioMixer::run()`'s own
  `setpriority(PRIO_PROCESS, gettid(), -5)` call - nowhere else in this codebase touches thread
  priority. The pinned thread was `AudioMixer` itself, the whole time, unaffected by any of the
  five receiver-side fixes above because none of them touch it.

  Its loop ended each iteration with `std::this_thread::yield()`, not a real sleep - a
  deliberate choice (see the code comment removed by this fix) to avoid WSL2/Hyper-V's coarse
  timer-coalescing turning short sleeps into ~100ms stalls, a real problem in a different
  deployment context but not scanner2's actual bare-metal Linux. `yield()` doesn't block; it
  just tells the scheduler "let someone else go if they want to," and combined with this
  thread's elevated priority and near-continuous readiness (it's back in the run queue the
  instant it yields), Linux's CFS scheduler kept rescheduling it almost continuously instead of
  giving other threads - including the receivers' own DSP block threads - a real chance to run,
  on a container with 300+ total threads sharing 4 cores. That's a busy-spin, not a wait,
  regardless of how little actual computation the loop body does per iteration (the assumption
  the elevated-priority design leaned on). Two standalone GNU Radio test programs confirmed
  `mute_ff` and `pwr_squelch_cc` (`gate=false`) both behave correctly under this hypothesis too
  - continuous zero-filled output, no dropped samples when muted/squelched - ruling those out as
  contributing to the "starved" (buffer genuinely empty, not just muted) symptom before landing
  on the scheduling explanation. Replaced with a real `sleep_for(1ms)`, well within the now-300ms
  latency margin above, which actually deschedules the thread for that duration instead of
  immediately re-queuing it.

  Confirmed on real hardware afterward: the CPU fix worked exactly as measured (the previously
  pinned thread dropped to 0%, overall idle CPU jumped from ~33% to ~72%), and separately, the
  audio itself sounded fine - the chronic "starved" percentage this whole investigation had been
  chasing turned out not to correspond to an audible problem *at the time* (this was all pre-DMR;
  see below for a real starvation cause that DMR/P25 introduced later). But the container was now
  hitting
  `docker ps`'s restart counter (confirmed via `docker inspect --format
  'ExitCode={{.State.ExitCode}} RestartCount={{.RestartCount}}'`, `ExitCode=0`), with `Scanner:
  AudioMixer not alive - stopping` in the log immediately before every restart - the liveness
  watchdog above firing deliberately (a clean, intentional exit, not a crash) because
  `lastHeartbeat_` had genuinely gone stale for the full `kHeartbeatTimeoutSeconds` window. That
  timeout (5s) was calibrated for the old `yield()`-based loop, which stayed continuously "hot"
  and never really sleeps; now that the loop does a real `sleep_for()` each iteration (the fix
  above), it's subject to normal OS thread scheduling like everything else, and on a container
  with 300+ threads sharing 4 cores, occasional multi-second scheduling delays don't necessarily
  mean the thread is actually hung. Widened to 20s - still well below anything a user would
  perceive as the app being stuck, but enough margin that transient scheduling contention isn't
  mistaken for a genuine deadlock and doesn't force an unnecessary full restart.

  That still didn't stop the crash-restart cycle - the watchdog fired again with the wider
  timeout too, meaning the real stall was genuinely 20+ seconds, not "just" a few seconds of
  scheduling jitter. Rather than keep widening a timeout that was clearly treating a symptom,
  `AudioMixer::run()`'s loop got temporary per-phase timing instrumentation (logging whenever a
  single iteration takes >200ms, broken down by read/mix/send phase) to get a direct answer.
  That immediately pointed at `AudioOutputWebsocket::send()`: unlike every other `AudioOutput`
  (including `AudioOutputIcecast`, which defers its own network I/O to a dedicated thread for
  exactly this reason), it called `WsStream::write()` - a synchronous socket write with no
  timeout configured - directly on `AudioMixer`'s own thread, once per connected client, per
  frame. A single slow or unresponsive WebSocket client (a browser tab left open but not
  reading, a laptop that went to sleep mid-connection, ...) could block that call indefinitely,
  freezing every receiver's audio at once and eventually tripping the liveness watchdog - the
  code's own comment on the graceful-close path even already warned about this exact failure
  mode, just hadn't been applied to the write in `send()`. Fixed by splitting `send()` the same
  way `AudioOutputIcecast` already does: it now only enqueues frames (`frameQueue_`, a quick
  mutex-guarded push, never blocks), and a new dedicated `writerThread_` drains that queue and
  performs the actual writes - so a stuck client can no longer stall anything upstream of it.
  That writer thread snapshots the client list under `clientsMutex_` and releases the lock
  *before* writing, rather than holding it for the whole write - holding it across a blocking
  write would mean `close()` (which needs the same lock to close client sockets and unstick a
  hung write during shutdown) would itself deadlock waiting on the very write it's trying to
  interrupt.
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

### FM noise squelch (reference-band hiss discriminator)

Real-hardware use surfaced a distinct problem from the debounce case above: audible "pops" -
short bursts of noise that reach the speaker even though nothing was actually transmitting. These
showed up on several FM/NFM channels with adaptive squelch margins anywhere from 6-19dB, and
happened while a receiver was parked on a channel, not just during a scan-window hop. A recording
of one such pop, captured and analyzed (windowed RMS envelope + a DFT of the ~2000-sample burst,
both done without any DSP library, just Python's stdlib `wave`/`audioop`/`cmath`), showed the pop
was a genuinely broadband noise burst - not a single spurious tone (which would have pointed at a
filter-design regression) - that happened to clear the power/adaptive squelch threshold and
persist past `SQUELCH_DEBOUNCE_SECONDS`. That's the crux of the problem: debounce alone can't
distinguish a real short transmission from a noise burst of similar duration, because duration is
the only thing it looks at.

FM has a discriminator that power/duration-based squelch doesn't use at all: the *capture effect*.
An FM demodulator's output on pure noise (no carrier captured) has a characteristic noise
spectrum that rises with frequency ("triangular" hiss, strongest in the last few kHz below the
audio filter's cutoff); a demodulator that has captured a real signal suppresses that hiss,
regardless of how weak or strong the underlying carrier is, because FM discriminators are
inherently more sensitive to the strongest instantaneous signal present. AM has no equivalent
effect, so this is FM/NFM-only, matching `analog::ctcss_squelch_ff`'s FM-only positioning above.

Implementation, in `ChannelBlockFM`:
- A reference band-pass filter (`blockNoiseRefFilter_`, ~4-7kHz) taps the same pre-deemphasis
  quadrature-demodulator output the audio chain uses, in parallel with it - it doesn't sit inline,
  so it can't add latency or artifacts to the audio path itself.
- That band's power is measured the same cheap way the RSSI chain measures RF power, but for a
  real (not complex) signal: self-multiplying the signal against itself via `gr::blocks::multiply`
  wired to the *same* source on both input ports computes x² without needing a separate squaring
  block, then a low-pass average (`blockNoiseRefLowPass_`) and a `Mag2ToPowerBlock` callback yield
  a live `noiseRefLevel_dBFS_` reading, mirroring the existing RSSI/noise-floor telemetry.
- `getStatus()`'s unmuted decision becomes a three-way AND: power/adaptive squelch (RF chain) AND
  CTCSS (if configured) AND `noiseRefLevel_dBFS_ <= noiseSquelchThreshold_dB` (if configured) - a
  channel with no `noiseSquelchThreshold_dB` set behaves exactly as before (opt-in, no change to
  existing channels).
- Deliberately independent of `squelchThreshold`/`squelchNoiseMargin_dB`: it's measuring the
  *shape* of the demodulated noise, not its level, so it can catch a broadband burst that's loud
  enough to already be above the power squelch (which is exactly the pops case above) without
  needing the power squelch tightened - tightening the power squelch instead would delay picking
  up real weak signals, which the user explicitly did not want traded away.
- Configured per-channel via `noiseSquelchThreshold_dB` (nullable `REAL` column, `PATCH
  /api/channels/{id}` and the `ChannelSetNoiseSquelchThreshold` WS message), same optional/nullable
  pattern as `ctcssToneHz` and `squelchNoiseMargin_dB`. Web UI: a "Noise Squelch (dB)" field next
  to CTCSS on both the main control page and the channel-settings table.
- The live reading itself (`noiseRefLevel_dBFS_`) is surfaced as `noiseRefLevel` in
  `ChannelStatusUpdate`/`ChannelStatus` WS broadcasts and shown alongside RSSI/noise-floor/volume
  in the main page's active-channel list (`NoiseRef: ...`), the same telemetry pattern
  `updateRSSI()`/`updateVolume()` already use - so a threshold can be picked by watching the real
  number on a channel that's been popping (quiet vs. captured-signal levels) instead of guessing.
  Lives on `ChannelBlockBase` (not `ChannelBlockFM`) so `reportStatus()` can include it generically;
  only `ChannelBlockFM` ever writes it, and `ChannelBlockEAS` copies it from its internal FM block
  the same way it already does for rssi/noiseFloor/volume - unset (`null`) for AM/NOAA-without-FM-
  telemetry or before the reference band's first measurement.

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

This settle delay is a real, measurable cost: the receiver's *entire* flowgraph - not just the
window being hopped away from - produces zero audio samples for its whole duration, since the
RF selector is already pointed at the discard port and the audio selector hasn't been
re-pointed at the new window yet (see `startWindow()`). A scanner that spends much of its time
round-robin-hopping through idle windows (the normal case when there's more configured
channels than fit in one window and nothing's currently transmitting) pays this cost on nearly
every hop, which showed up on real hardware as `AudioMixer`'s "falling behind real time"
warning, chronically - not from CPU load, from this dead time.

Upstream's calibrated default pairs `buffers=8` with a 20ms pause. This receiver type requests
`buffers=32` instead (4x deeper - see the `streamArgs` comment in `SoapyReceiver.cpp` for why:
the theory was surviving USB completion latency on a virtualized passthrough, e.g. WSL2's
usbipd) - but real-hardware logs showed that override was never actually taking effect:
SoapyRTLSDR always logs its own default ("Allocating 15 zero-copy buffers"), regardless of the
streamArgs value passed. `kRtlSdrTuneSettleMs` had been scaled to match the requested (but
never real) 32-buffer depth - 80ms, 4x upstream's 20ms - so every hop was paying for queue
depth that didn't exist. Rescaled to the actual 15-buffer depth instead (roughly `15/8 * 20ms`,
~40ms) - since the real fix for excess hop-driven dead time is shrinking that cost accurately,
not stretching scan dwell time to dilute it (which would trade away scan responsiveness to
compensate for a miscalibrated constant). If `buffers=32` needs to genuinely take effect (or
"invalid squelch breaks" reappear), revisit both together - `kRtlSdrTuneSettleMs` assumes the
15-buffer default is what's actually in use. Not yet exposed as a per-receiver config knob the
way upstream's `tunePause` is, since no hardware has needed that granularity yet.

### Simplifications vs. the Python version (documented in code comments too)

- Input channelization uses a two-stage time-domain FIR split (`ChannelBlockBase::
  splitDecimation()`, falling back to a single stage at low decimation ratios - see "Adaptive
  squelch and squelch debounce" above for why this was added) rather than the Python version's
  FFT-filter split for very high decimation ratios. Both address the same real-time-compute
  problem at high decimation ratios; revisit if profiling ever shows the time-domain approach
  still isn't enough for a particular receiver's sample rate.
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
  "Adaptive squelch" above), `noiseSquelchThreshold_dB` (number or `null` to disable - FM/NFM
  only, see "FM noise squelch" above), `ctcssToneHz` (number or `null` to disable), `dmrSlot`
  (1 or 2 - DMR only, structural: rebuilds the window like `freq_hz`/`mode` - see "DMR/P25
  digital decode" below), `dmrTalkgroupFilter` (number or `null` to unmute for any talkgroup -
  DMR only, hot), `audioGain_dB`, `dwellTime_s`, `mute`, `solo` (`true`/`false`/`null`), `hold`,
  `forceActive`, `enabled`, `disableUntil` (unix seconds).
- `POST /api/channels` - body: `{freq_hz, label?, mode?, audioGain_dB?, dwellTime_s?,
  squelchThreshold?, squelchNoiseMargin_dB?, noiseSquelchThreshold_dB?, ctcssToneHz?, dmrSlot?,
  dmrTalkgroupFilter?}` -> `{id}`.
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
`ChannelForceActive`, `ChannelSetSquelch`, `ChannelSetSquelchNoiseMargin`,
`ChannelSetNoiseSquelchThreshold`, `ChannelSetCtcss`, `ChannelSetDmrTalkgroupFilter`,
`ChannelSetAudioGain`, `ChannelSetDwellTime`.
Also accepts `Ping` (empty `data`), which does nothing besides getting the usual `Ack` back - a
periodic client-side keepalive (`app.js`'s `connectWS()`, every 20s) exists because the control
connection otherwise sits idle between actual config/status changes, which on real hardware was
found to get silently dropped by a reverse proxy sitting in front of this app well under any
timeout a browser talking to it directly would ever hit - the backend itself never saw a problem
(no crash, no restart), just repeated client-side "disconnected - retrying" cycles.

Note: the Python web UI had grown a PIN-based "listen only vs. control" access gate
(`SDRSCANNER_CONTROL_PIN`). That's not reimplemented here yet - every connected client can
control the scanner. If you need that back, it'd be a reasonable addition to `HttpServer`, or
handle it at a reverse-proxy layer in front of this.

## DMR/P25 digital decode

AMBE/IMBE voice decode goes through **DSDcc** (`f4exb/dsdcc`), a C++11 library wrapping
**mbelib** (`f4exb/mbelib`) that adds DMR/P25 Phase 1 frame sync/deframing on top - the same
reverse-engineered codec family OpenWebRX+'s digiham and SDRTrunk's jmbe use, run entirely in
software (no AMBE hardware dongle). Neither library is packaged in apt; `Dockerfile` builds both
from source before the main (apt-only) build stage - see "Local (fast dev loop)" above for the
equivalent manual steps.

**Architecture**: `ChannelBlockDMR` (`src/dsp/ChannelBlockDMR.{h,cpp}`) channelizes the shared
window RF input down to a 48kHz discriminator stream (C4FM/4FSK quadrature demod, same technique
`ChannelBlockFM` uses for analog FM - no de-emphasis/CTCSS, those are FM-broadcast-specific) and
feeds it sample-by-sample into `DsdccDecodeBlock` (`src/dsp/DsdccDecodeBlock.{h,cpp}`), a thin
GNU Radio wrapper around `DSDcc::DSDDecoder` that polls its decoded-audio output each `work()`
call. mbelib only actually decodes audio in bursts (DSDcc has to first sync to a frame), but
`DsdccDecodeBlock` always produces output at a fixed rate regardless (zero-filling whenever
nothing's been decoded yet) - it's a `gr::block`, not `gr::sync_block`, only because of the
48kHz-in/8kHz-out ratio and to poll DSDcc's decoder state each call, not because its output rate
is actually variable. This matters more than it sounds: see the real production outage this
caused, documented below.

**DMR is dual-timeslot**: TS1/TS2 are configured as two independent `ChannelConfig` rows at the
same `freq_hz` (own mute/hold/status/talkgroup filter per slot - see `dmrSlot`/
`dmrTalkgroupFilter` below), sharing one underlying C4FM front end + `DSDDecoder` instance rather
than each independently demodulating the same RF (DSDcc already decodes both TDMA slots from one
input - `getAudio1()`/`getAudio2()`). Whichever slot's `ChannelBlockDMR` is constructed first for
a frequency (`ScanWindow::buildChannelBlock`'s `freq_hz`-keyed lookup) builds the real front end/
decoder; the second one discards its own copy of the window's RF stream into a `null_sink` (GNU
Radio requires every `hier_block2` boundary port connected internally) and taps the shared
decoder's other output port instead - validated as a supported GNU Radio pattern (a plain
`gr::block`, unlike a `gr::hier_block2`, can be connected to from two independent parent
`hier_block2`s) with a standalone synthetic flowgraph before committing to this design.

**Config**: `ChannelMode::DMR`, plus DMR-only `ChannelConfig` fields `dmrSlot` (1 or 2 - which
timeslot; **structural**, since changing it changes the block's port wiring/owner-vs-shared-tap
role, so it goes through the same window rebuild as `freq_hz`/`mode`, not a hot setter) and
`dmrTalkgroupFilter` (optional talkgroup ID allow-list of one - unset unmutes for any talkgroup
on that slot; hot-settable via `PATCH`/`ChannelSetDmrTalkgroupFilter`). DMR channels no longer
require the window's RF sample rate to be a whole multiple of 48000Hz - `ChannelBlockDMR` corrects
any remainder with its own internal rational resampler (see the constructor), so any RF sample
rate that covers the channel's own bandwidth works, matching every other mode's requirements. This
matters because the RF sample rate is a receiver-level property shared by every window on that
receiver (see `Scanner::buildWindows()`) - it must never be chosen *for* DMR at the cost of every
other window's bandwidth.

**Fixed bug: a DMR/P25 channel with no active traffic used to silently stall its entire window's
audio, not just its own.** `ScanWindowBlock::mixerAdd_` (`src/dsp/ScanWindow.cpp`) sums every
channel in a window via a synchronous `gr::blocks::add_ff`, which can't produce *any* output
until every one of its connected ports has data. `DsdccDecodeBlock` used to only `produce()`
however many audio items DSDcc actually had decoded that call - zero, for as long as it wasn't
mid-voice-frame, which in production (real traffic is intermittent) is most of the time. Since
`buildWindows()` groups channels purely by frequency proximity, not by mode, a DMR/P25 channel
sharing a window with FM/AM channels would drag the whole window's audio down with it whenever it
went quiet - and the backpressure eventually propagated far enough upstream to freeze every
channel's RSSI/volume/noise-floor telemetry too, since GNU Radio's scheduler stops calling a
block once its output buffer fills waiting on a downstream consumer that never drains. On real
hardware this showed up as `AudioMixer` reporting ~100% starvation continuously (not the
fluctuating 30-68% of the earlier, unrelated pre-DMR investigation above) on every receiver, for
as long as any currently-tuned window contained a quiet DMR/P25 channel - i.e. most of the time.
Fixed by making `DsdccDecodeBlock::general_work()` always produce a fixed-rate output stream
(zero-filled when nothing's been decoded yet, exactly like every other channel mode's audio gate
already does when squelched) - see `tests/test_dsdcc_decode_block.cpp`.

**Fixed regression from the fix above: input consumption was throttled by output buffer space,
which could break a lock DSDcc had genuinely already achieved.** The first version of the fix
capped how many input samples got fed to `decoder_.run()` each call at
`noutput_items * kInputPerAudioSample` - meaning whenever downstream output buffer space was
limited for a call, the decoder fell behind the real-time 48kHz input rate. That's a problem
regardless of whether GNU Radio's own buffering could absorb the delay losslessly in principle:
falling behind risks backpressure propagating all the way back to the real-time RF source, which
can genuinely drop hardware samples - the same failure mode as the round-robin discontinuity bug
below, just reached a different way. Confirmed on real hardware, keying a known-active transmission
end to end: sync achieved a clean, low-error lock (`dataBS=2`, well inside DSDcc's 2/24 tolerance)
and then lost it well under a second later, mid-transmission, with nothing that should have
interrupted it. Fixed by decoupling input consumption from output space entirely - always consume
everything available at the real rate, and let any output backlog wait in `pending1_`/`pending2_`
for a later call instead of ever throttling input.

**Fixed the actual root cause of "syncs cleanly then loses lock in under a second": DSDcc's own
built-in squelch-timeout was misfiring on real signal.** The input-throttling fix above was a
real, legitimate bug (fixed regardless), but confirmed *not* the explanation for this symptom -
a real hardware test with the PTT held over 10 continuous seconds still lost a clean lock
(`voiceMS=2`) in well under a second. The actual cause, found by reading DSDcc's source directly:
`DSDDecoder::run()` (upstream `dsd_decoder.cpp`) treats a literal `0` input sample as "an external
analog squelch just closed" - after `DSD_SQUELCH_TIMEOUT_SAMPLES` (960, 20ms at 48kHz) consecutive
exact zeros, it discards whatever sync it has and goes back to hunting. That's correct behavior
for DSDcc's classic calling convention (`rtl_fm | dsd`, where an external squelch gate feeds
literal zeros during real silence) - but `DsdccDecodeBlock` feeds it raw, unsquelched discriminator
output, where a real, actively-transmitting signal can still legitimately produce a sample that
quantizes to exactly 0 (a fade, a TDMA idle-slot gap, low-amplitude noise between symbols) without
meaning the transmission stopped. Fixed by nudging any would-be-zero sample to the smallest
representable nonzero value before handing it to `decoder_.run()` - amplitude-wise negligible
(~1/32767 of full scale), but keeps DSDcc's own timeout from ever firing on real signal.

**Fixed bug: DMR/P25 channels never actually got a real chance to sync, even with a strong
signal, because the round-robin scanner scheduler was cutting off their RF exposure every
~100ms.** Unlike FM/AM's power squelch (which only needs a fraction of a second to sample carrier
level), DSDcc's frame sync needs *continuous* discriminator samples across several consecutive
TDMA bursts to lock on at all - any gap and it has no way to know time passed, so the next
fragment starts the sync hunt over from an arbitrary phase relationship to the transmitter's TDMA
clock. `ChannelBlockBase::getMinimumScanTime()` defaults to 0.1s (sized for analog squelch,
`ChannelBlockBase.h`), and neither `ChannelBlockDMR` nor `ChannelBlockP25Voice` overrode it - so
`SoapyReceiver::checkCurrentWindow()` would consider a DMR/P25 window eligible to hop away to the
next window in the rotation after just 100ms, regardless of whether DSDcc had any chance to
sync yet. Diagnosed via a real-hardware capture (with the sync-mismatch diagnostic logging above)
that showed sync constantly flapping between correct repeater framing (`DMRDataP`/`DMRVoiceP`)
and DSDcc's uncertain fallback (`DMRDataMS`/`DMRVoiceMS`), never holding past the same second -
initially suspected as a weak-signal issue, ruled out once confirmed the repeater's antenna was
~20 feet from the receiver (about as strong a signal as real-world DMR reception gets). Fixed by
overriding `getMinimumScanTime()` to 1.5s on both classes, giving the scheduler a guaranteed
continuous dwell long enough for DSDcc to actually get a fair shot - see
`tests/test_channel_dmr.cpp`/`tests/test_channel_p25voice.cpp`'s `getMinimumScanTime()` checks.

**Known caveats** (unverified against real DMR/P25 RF - no SDR hardware or signal generator
available in the sandbox this was built in):
- The C4FM discriminator gain assumes a DMR peak deviation of 1944Hz (`kDmrPeakDeviationHz` in
  `ChannelBlockDMR.cpp`) and a fixed int16 scale factor feeding `DSDDecoder::run()`
  (`DsdccDecodeBlock.cpp`) - DSDcc's symbol-level tracking auto-adapts rather than needing exact
  calibration, but both are flagged for real-world tuning if decode quality is poor.
- Upstream DSDcc's `DSDDecoder::getOpts()`/`getState()` are commented out, and `DSDDMR` only
  exposes the decoded talkgroup as formatted text (`getSlot0Text()`/`getSlot1Text()`), not a
  queryable numeric field - `dmrTalkgroupFilter` parses it back out of that text (see
  `ChannelBlockDMR::currentTalkgroup()`). A small vendored patch (uncommenting those getters,
  exposing the target address directly) would be more robust if DSDcc gets version-bumped later.
- Real DMR decode correctness (does audio actually come out right) needs a captured IQ recording
  or real hardware to verify - `tests/test_channel_dmr.cpp` covers the shared-decoder wiring,
  slot independence, and that the flowgraph runs against noise without crashing, not audio
  correctness.
- **DMR sync sometimes still lands on DSDcc's "MS" (direct-mode) sync type instead of "BS"
  (repeater) sync type against a confirmed conventional 2-slot MOTOTRBO-style repeater**, even
  after the `getMinimumScanTime()` fix above gives it a continuous window to lock on. This
  matters because DSDcc's DMR decoder (`dmr.cpp`'s `processVoiceFirstHalfMS()`, upstream, not
  this project's code) hardcodes all MS-framed voice to internal "slot 1" regardless of the
  actual configured `dmrSlot` - "no CACH, only one slot in MS" per its own comment - so a channel
  configured for `dmrSlot=2` never reports ACTIVE even while genuinely decoding real audio, and
  the audio itself never reaches that channel's output port either (statically wired to
  "slot 2" data at construction, which MS-framed traffic never populates). `native/dsdcc-
  diagnostics.patch` (applied in both `Dockerfile` and `native-ci.yml`'s DSDcc build steps)
  exposes DSDcc's internal per-pattern sync-mismatch counts via
  `DSDDecoder::getDmrDataBsSyncErrors()`/`getDmrVoiceBsSyncErrors()`/etc. (0-24 dibits, tolerance
  2 - see `dsd_sync.cpp`), logged by `DsdccDecodeBlock::logSyncTypeChange()` alongside every
  sync-type transition.
- **Still not confirmed working end-to-end against real DMR traffic, and the evidence from the
  most recent real-hardware capture (all fixes above in place, PTT held for a sustained
  transmission) points at "not really syncing at all" rather than a lingering scheduling issue.**
  Across that entire capture, every winning DMR sync match (`DMRDataP`/`DMRVoiceP`/`DMRDataMS`/
  `DMRVoiceMS`) landed at exactly 2 mismatched dibits out of 24 - the maximum DSDcc's tolerance
  allows - with a single exception at 1 mismatch and *none* at 0. A genuine repeater lock, once
  established, should track the transmitted sync word cleanly (0-1 mismatches) for as long as the
  signal stays up; matches that only ever squeak in exactly at the tolerance boundary, never
  better, are the statistical signature of a correlator matching noise (or some other signal
  entirely - the same capture also showed extensive matches against DSDcc's NXDN sync pattern),
  not a real fixed sync word. Consistent with this, none of the DMR-type sync events in that
  capture chained into consecutive ~360ms DMR superframes (see the `DMR_VOX_SUPERFRAME_LEN`
  finding above) - each was an isolated single event separated from the next by seconds of `None`/
  other-pattern noise, not the tight back-to-back cadence a real held call should produce. Given
  the repeater's strong, close-range signal (already ruled out as a weak-signal explanation for
  earlier symptoms), the next thing to verify is further upstream of DSDcc entirely: whether
  146955000's C4FM front end (`ChannelBlockDMR`'s freq-xlating filter/channel filter/quad-demod
  chain) is actually producing clean 4800-baud FSK4 discriminator symbols at all for this specific
  repeater (frequency/offset accuracy, deviation, filter bandwidth), ideally checked by capturing
  raw IQ during a confirmed transmission and inspecting/decoding it offline with a known-good tool
  independent of this codebase.
- Also fixed alongside the above (not the root cause, but was actively corrupting these
  diagnostic logs into an apparently self-contradictory sequence): `DsdccDecodeBlock`'s
  `logLabel` was just the channel frequency, so if the same frequency is configured as a channel
  on more than one receiver (each gets its own independent front end + `DsdccDecodeBlock`
  instance - see `ScanWindow::buildChannelBlock`), their sync-transition logs interleaved under an
  identical `[freq]` tag, reading as one FSM that transitions *from* a state it was never logged
  as entering. Fixed by appending a process-wide instance counter to the label
  (`[freq#instanceId]`) so concurrent instances at the same frequency are always distinguishable
  in the log.

## Explicitly deferred

- **SSB channel mode** - same `ChannelBlockBase` extension point as
  `ChannelBlockFM`/`ChannelBlockAM`/`ChannelBlockEAS`; a straightforward follow-up.
- **wxPython GUI** - dropped in favor of the web UI (the Python repo's own README already
  listed this as a TODO).
- **P25 Phase 1 (conventional)** - same DSDcc/mbelib path as DMR (see "DMR/P25 digital decode"
  above) with `setDecodeMode(DSDDecodeP25P1, true)` instead, one audio stream (no timeslot
  split) - a straightforward follow-up on top of `ChannelBlockDMR`'s plumbing.
- **P25 Phase 1 trunking** (e.g. a county-wide trunked system) - the substantially bigger lift:
  DSDcc has no TSBK/MBT trunking decode at all (only post-sync voice-frame heuristics), so
  following a control channel (channel grants, `IDEN_UP` frequency-band table) needs a
  standalone decoder with no library shortcut, plus a new receiver-role concept (one receiver
  pinned to the control channel, another preemptable from normal conventional scanning onto a
  voice grant) - `Scanner::buildWindows()`/`getNextScanWindowId()` have no notion of either
  today. Deferred entirely for now.
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
