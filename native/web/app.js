// sdr-scanner (native) web GUI - adapted from the Python version's web_static/app.js.
// Removed: PIN/role elevation (that was a Python-side add-on, not reimplemented here yet -
// every connected client can control the scanner, matching the pre-PIN Python baseline).
// Added: squelch / CTCSS / audio gain controls, wired to the new ChannelSetSquelch /
// ChannelSetCtcss / ChannelSetAudioGain WS message types.

let ws = null;
let selectedId = null;

const cfgById = new Map();
const statusById = new Map();
const lastSeen = new Map();

const activeList = document.getElementById("activeList");
const cfgTableBody = document.querySelector("#cfgTable tbody");
const connStatus = document.getElementById("connStatus");
const logEl = document.getElementById("log");

const selMeta = document.getElementById("selectedMeta");
const btnHold = document.getElementById("btnHold");
const btnSolo = document.getElementById("btnSolo");
const btnMute = document.getElementById("btnMute");
const btnForce = document.getElementById("btnForce");
const btnEnable = document.getElementById("btnEnable");
const btnDisable1h = document.getElementById("btnDisable1h");

const squelchInput = document.getElementById("squelchInput");
const squelchMarginInput = document.getElementById("squelchMarginInput");
const noiseSquelchInput = document.getElementById("noiseSquelchInput");
const ctcssInput = document.getElementById("ctcssInput");
const gainInput = document.getElementById("gainInput");
const btnSquelchApply = document.getElementById("btnSquelchApply");
const btnSquelchMarginApply = document.getElementById("btnSquelchMarginApply");
const btnSquelchMarginClear = document.getElementById("btnSquelchMarginClear");
const btnNoiseSquelchApply = document.getElementById("btnNoiseSquelchApply");
const btnNoiseSquelchClear = document.getElementById("btnNoiseSquelchClear");
const btnCtcssApply = document.getElementById("btnCtcssApply");
const btnCtcssClear = document.getElementById("btnCtcssClear");
const btnGainApply = document.getElementById("btnGainApply");

const AUDIO_WS_PORT = (window.AUDIO_WS_PORT != null) ? Number(window.AUDIO_WS_PORT) : 8123;
const AUDIO_SAMPLE_RATE = (window.AUDIO_SAMPLE_RATE != null) ? Number(window.AUDIO_SAMPLE_RATE) : 16000;

function log(line) {
  const ts = new Date().toISOString();
  if (logEl) logEl.textContent = `[${ts}] ${line}\n` + logEl.textContent;
}

function ensureAudioControls() {
  // panel_ui builds its own audio bar with the same element IDs, sized and placed for a
  // 480x320 kiosk screen rather than crammed into <header> - see panel_ui/index.html.
  if (document.body.hasAttribute("data-external-audio-controls")) return;

  const header = document.querySelector("header");
  if (!header) return;
  if (document.getElementById("audioControls")) return;

  const box = document.createElement("div");
  box.id = "audioControls";
  box.style.display = "flex";
  box.style.alignItems = "center";
  box.style.gap = "10px";

  const label = document.createElement("div");
  label.textContent = "Audio:";
  label.style.fontWeight = "600";

  const btn = document.createElement("button");
  btn.id = "audioToggle";
  btn.textContent = "Start";
  btn.className = "btn";

  const volLabel = document.createElement("div");
  volLabel.textContent = "Vol";
  volLabel.style.fontSize = "12px";

  const vol = document.createElement("input");
  vol.id = "audioVolume";
  vol.type = "range";
  vol.min = "0";
  vol.max = "1";
  vol.step = "0.01";
  vol.value = "0.8";

  const status = document.createElement("div");
  status.id = "audioStatus";
  status.textContent = "stopped";
  status.style.fontSize = "12px";

  box.appendChild(label);
  box.appendChild(btn);
  box.appendChild(volLabel);
  box.appendChild(vol);
  box.appendChild(status);
  header.appendChild(box);
}

// Audio state machine, driven by actual observable signals rather than "did we call
// connect/start" bookkeeping - so a page can show a genuinely accurate "not playing" indication
// (the panel_ui request this exists for) instead of just reflecting which function was last
// called. "playing" requires all three of: the toggle is on, the AudioContext isn't suspended
// (browsers block audio output without a user gesture - see the tap-to-resume handler in
// init()), and a PCM frame has actually arrived recently (catches a connected-but-silent
// upstream, not just a dropped WebSocket).
let lastPcmFrameAt = 0;
const AUDIO_SILENCE_WATCHDOG_MS = 4000;

function currentAudioState() {
  if (!audioPlaying) return "off";
  if (!audioWs || audioWs.readyState !== WebSocket.OPEN) return "connecting";
  if (audioCtx && audioCtx.state !== "running") return "blocked";
  if (Date.now() - lastPcmFrameAt > AUDIO_SILENCE_WATCHDOG_MS) return "silent";
  return "playing";
}

// Updates every element that opted in via data-audio-indicator (state only, e.g. a colored dot)
// or data-audio-status-text (human label) - lets a page show as much or as little of this as it
// wants without app.js knowing about any particular page's layout. #audioStatus is kept as a
// plain text target too, for back-compat with the original single-line status display.
function refreshAudioIndicator() {
  const state = currentAudioState();
  const bufferedS = playbackBuffer ? (playbackBuffer.available() / AUDIO_SAMPLE_RATE) : 0;
  const label = {
    off: "stopped",
    connecting: "connecting...",
    blocked: "tap to enable audio",
    silent: "no audio",
    playing: `playing (${bufferedS.toFixed(2)}s buffered)`,
  }[state];

  const statusEl = document.getElementById("audioStatus");
  if (statusEl) statusEl.textContent = label;
  document.querySelectorAll("[data-audio-status-text]").forEach(el => { el.textContent = label; });
  document.querySelectorAll("[data-audio-indicator]").forEach(el => { el.dataset.audioState = state; });
}

function setConn(ok) {
  if (!connStatus) return;
  connStatus.textContent = ok ? "connected" : "disconnected";
  connStatus.style.background = ok ? "rgba(16,185,129,0.25)" : "rgba(239,68,68,0.25)";
}

function send(msg) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify(msg));
}

function freqMHz(hz) {
  if (hz === undefined || hz === null) return "";
  return (hz / 1e6).toFixed(3);
}

function badge(text, cls) {
  const b = document.createElement("span");
  b.className = "badge " + (cls || "");
  b.textContent = text;
  return b;
}

function renderConfigTable() {
  if (!cfgTableBody) return;
  cfgTableBody.innerHTML = "";
  const rows = Array.from(cfgById.values()).sort((a, b) => (a.freq_hz || 0) - (b.freq_hz || 0));
  for (const c of rows) {
    const tr = document.createElement("tr");
    tr.addEventListener("click", () => selectChannel(String(c.id)));
    tr.innerHTML = `
      <td>${c.label ?? ""}</td>
      <td>${freqMHz(c.freq_hz)}</td>
      <td>${c.mode ?? ""}</td>
      <td>${c.squelchThreshold ?? ""}</td>
      <td>${c.ctcssToneHz ?? "off"}</td>
      <td>${c.enabled ? "yes" : "no"}</td>
      <td>${c.hold ? "yes" : "no"}</td>
      <td>${c.mute ? "yes" : "no"}</td>
      <td>${c.solo === true ? "yes" : (c.solo === false ? "no" : "")}</td>
      <td>${c.forceActive ? "yes" : "no"}</td>
    `;
    cfgTableBody.appendChild(tr);
  }
}

function renderActiveList() {
  if (!activeList) return;

  activeList.innerHTML = "";
  const now = Date.now() / 1000;
  const items = [];

  for (const [id, st] of statusById.entries()) {
    const seen = lastSeen.get(id) || 0;
    if (now - seen > 15) continue;
    items.push([id, st]);
  }

  items.sort((a, b) => {
    const sa = a[1], sb = b[1];
    const aa = (sa.status || "") === "ACTIVE" ? 1 : 0;
    const bb = (sb.status || "") === "ACTIVE" ? 1 : 0;
    return bb - aa;
  });

  for (const [id, st] of items) {
    const c = cfgById.get(id) || {};
    const div = document.createElement("div");
    div.className = "strip" + (selectedId === id ? " selected" : "");
    div.addEventListener("click", () => selectChannel(id));

    const top = document.createElement("div");
    top.className = "top";

    const left = document.createElement("div");
    left.innerHTML = `<div class="label">${c.label ?? id}</div><div class="freq">${freqMHz(c.freq_hz)} MHz</div>`;

    const badges = document.createElement("div");
    badges.className = "badges";
    badges.appendChild(badge(st.status || "UNKNOWN", st.status === "ACTIVE" ? "active" : ""));
    if (c.hold) badges.appendChild(badge("HOLD", "hold"));
    if (c.forceActive) badges.appendChild(badge("FORCE", "force"));
    if (c.mute) badges.appendChild(badge("MUTE", "mute"));
    if (c.solo === true) badges.appendChild(badge("SOLO", ""));

    top.appendChild(left);
    top.appendChild(badges);

    const meters = document.createElement("div");
    meters.className = "meters";
    meters.textContent = `RSSI: ${st.rssi ?? ""}   NF: ${st.noiseFloor ?? ""}   VOL: ${st.volume ?? ""}` +
      (st.noiseRefLevel != null ? `   NoiseRef: ${st.noiseRefLevel}` : "");

    div.appendChild(top);
    div.appendChild(meters);
    activeList.appendChild(div);
  }
}

function updateSelectedButtons() {
  if (!selMeta) return;

  if (!selectedId) {
    selMeta.textContent = "None";
    [btnHold, btnSolo, btnMute, btnForce, btnEnable, btnDisable1h, btnSquelchApply, btnSquelchMarginApply, btnSquelchMarginClear, btnNoiseSquelchApply, btnNoiseSquelchClear, btnCtcssApply, btnCtcssClear, btnGainApply].forEach(b => {
      if (!b) return;
      b.disabled = true;
      b.classList.remove("on");
    });
    return;
  }

  const c = cfgById.get(selectedId) || {};
  selMeta.textContent = `${c.label ?? selectedId}  •  ${freqMHz(c.freq_hz)} MHz  •  ${c.mode ?? ""}`;

  [btnHold, btnSolo, btnMute, btnForce, btnEnable, btnDisable1h, btnSquelchApply, btnSquelchMarginApply, btnSquelchMarginClear, btnNoiseSquelchApply, btnNoiseSquelchClear, btnCtcssApply, btnCtcssClear, btnGainApply].forEach(b => {
    if (!b) return;
    b.disabled = false;
  });

  btnHold?.classList.toggle("on", !!c.hold);
  btnMute?.classList.toggle("on", !!c.mute);
  btnForce?.classList.toggle("on", !!c.forceActive);
  btnSolo?.classList.toggle("on", c.solo === true);
  btnEnable?.classList.toggle("on", !!c.enabled);

  if (squelchInput) squelchInput.value = c.squelchThreshold ?? "";
  if (squelchMarginInput) squelchMarginInput.value = c.squelchNoiseMargin_dB ?? "";
  if (noiseSquelchInput) noiseSquelchInput.value = c.noiseSquelchThreshold_dB ?? "";
  if (ctcssInput) ctcssInput.value = c.ctcssToneHz ?? "";
  if (gainInput) gainInput.value = c.audioGain_dB ?? "";
}

function selectChannel(id) {
  selectedId = id;
  renderActiveList();
  updateSelectedButtons();
}

btnHold?.addEventListener("click", () => {
  const c = cfgById.get(selectedId); if (!c) return;
  send({ type: "ChannelHold", data: { id: selectedId, hold: !c.hold } });
});
btnMute?.addEventListener("click", () => {
  const c = cfgById.get(selectedId); if (!c) return;
  send({ type: "ChannelMute", data: { id: selectedId, mute: !c.mute } });
});
btnForce?.addEventListener("click", () => {
  const c = cfgById.get(selectedId); if (!c) return;
  send({ type: "ChannelForceActive", data: { id: selectedId, forceActive: !c.forceActive } });
});
btnSolo?.addEventListener("click", () => {
  const c = cfgById.get(selectedId); if (!c) return;
  send({ type: "ChannelSolo", data: { id: selectedId, solo: !(c.solo === true) } });
});
btnEnable?.addEventListener("click", () => {
  const c = cfgById.get(selectedId); if (!c) return;
  send({ type: "ChannelEnable", data: { id: selectedId, enabled: !c.enabled } });
});
btnDisable1h?.addEventListener("click", () => {
  const until = (Date.now() / 1000) + 3600;
  send({ type: "ChannelDisableUntil", data: { id: selectedId, disableUntil: until } });
});
btnSquelchApply?.addEventListener("click", () => {
  if (!selectedId || !squelchInput) return;
  const v = Number(squelchInput.value);
  if (Number.isFinite(v)) send({ type: "ChannelSetSquelch", data: { id: selectedId, squelchThreshold: v } });
});
btnSquelchMarginApply?.addEventListener("click", () => {
  if (!selectedId || !squelchMarginInput) return;
  const v = Number(squelchMarginInput.value);
  if (Number.isFinite(v)) send({ type: "ChannelSetSquelchNoiseMargin", data: { id: selectedId, squelchNoiseMargin_dB: v } });
});
btnSquelchMarginClear?.addEventListener("click", () => {
  if (!selectedId) return;
  send({ type: "ChannelSetSquelchNoiseMargin", data: { id: selectedId, squelchNoiseMargin_dB: null } });
});
btnNoiseSquelchApply?.addEventListener("click", () => {
  if (!selectedId || !noiseSquelchInput) return;
  const v = Number(noiseSquelchInput.value);
  if (Number.isFinite(v)) send({ type: "ChannelSetNoiseSquelchThreshold", data: { id: selectedId, noiseSquelchThreshold_dB: v } });
});
btnNoiseSquelchClear?.addEventListener("click", () => {
  if (!selectedId) return;
  send({ type: "ChannelSetNoiseSquelchThreshold", data: { id: selectedId, noiseSquelchThreshold_dB: null } });
});
btnCtcssApply?.addEventListener("click", () => {
  if (!selectedId || !ctcssInput) return;
  const v = Number(ctcssInput.value);
  if (Number.isFinite(v) && v > 0) send({ type: "ChannelSetCtcss", data: { id: selectedId, ctcssToneHz: v } });
});
btnCtcssClear?.addEventListener("click", () => {
  if (!selectedId) return;
  send({ type: "ChannelSetCtcss", data: { id: selectedId, ctcssToneHz: null } });
});
btnGainApply?.addEventListener("click", () => {
  if (!selectedId || !gainInput) return;
  const v = Number(gainInput.value);
  if (Number.isFinite(v)) send({ type: "ChannelSetAudioGain", data: { id: selectedId, audioGain_dB: v } });
});

async function loadSnapshot() {
  const r = await fetch("/api/state", { cache: "no-store" });
  const s = await r.json();

  for (const c of (s.channels || [])) cfgById.set(String(c.id), c);
  for (const st of (s.channelStatuses || [])) {
    const id = String(st.id);
    statusById.set(id, st);
    lastSeen.set(id, Date.now() / 1000);
  }

  renderConfigTable();
  renderActiveList();

  if (!selectedId && cfgById.size) {
    const first = cfgById.values().next().value;
    selectChannel(String(first.id));
  } else {
    updateSelectedButtons();
  }
}

function handleMsg(msg) {
  if (!msg || !msg.type) return;

  if (msg.type === "Error") {
    log(`error: ${msg.data?.error || "unknown"} (${msg.data?.messageType || ""})`);
    return;
  }

  if (msg.type === "Snapshot") {
    const d = msg.data || {};
    for (const c of (d.channels || [])) cfgById.set(String(c.id), c);
    for (const st of (d.channelStatuses || [])) {
      const id = String(st.id);
      statusById.set(id, st);
      lastSeen.set(id, Date.now() / 1000);
    }
    renderConfigTable();
    renderActiveList();
    updateSelectedButtons();
    return;
  }

  if (msg.type === "ChannelConfig") {
    const c = msg.data || {};
    cfgById.set(String(c.id), c);
    renderConfigTable();
    renderActiveList();
    updateSelectedButtons();
    return;
  }

  if (msg.type === "ChannelStatus") {
    const st = msg.data || {};
    const id = String(st.id);
    statusById.set(id, st);
    lastSeen.set(id, Date.now() / 1000);
    renderActiveList();
    return;
  }
}

function connectWS() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { setConn(true); log("ws connected"); };
  ws.onclose = () => { setConn(false); log("ws disconnected - retrying"); setTimeout(connectWS, 1000); };
  ws.onerror = () => { setConn(false); };
  ws.onmessage = (ev) => {
    try { handleMsg(JSON.parse(ev.data)); }
    catch { /* ignore */ }
  };
}

///
// Browser audio playback (int16 PCM over a separate WS, from the "websocket" AudioOutput)
//
// This used to schedule a separate AudioBufferSourceNode per incoming ~250ms chunk via
// src.start(scheduledTime). That's a well-known source of audible clicks/gaps: Web Audio only
// schedules start times to the nearest internal render-quantum boundary, so chaining many
// discrete buffer nodes back-to-back - even with otherwise-perfect timing - produces a click
// at every boundary (4/sec at 250ms chunks). That matched the reported symptom exactly: choppy
// audio that didn't improve with any server-side timing fix, and was still choppy with squelch
// forced open (i.e. on genuinely continuous, ungated audio - not squelch chatter, not mixer
// starvation, not delivery jitter).
//
// Fixed by not scheduling discrete buffers at all: incoming samples are pushed into a plain
// ring buffer, and a single continuously-running audio callback (ScriptProcessorNode) pulls
// from it every render cycle. There's no discrete start/stop boundary anywhere, so there's
// nothing for a click to happen at.

let audioWs = null;
let audioCtx = null;
let gainNode = null;
let processorNode = null;
let audioPlaying = false;
let playbackBuffer = null;
let playbackPrimed = false;

// ScriptProcessorNode is deprecated in favor of AudioWorklet, but remains broadly supported
// and is far simpler to wire up inline here (no separate module file, no secure-context/
// module-loading requirements). Fine for this use case - not low-latency interactive audio.
// Revisit with an AudioWorklet if that deprecation ever becomes a practical problem.
const PROCESSOR_BUFFER_SIZE = 1024;
const PLAYBACK_RING_SECONDS = 2.0; // ring buffer capacity (a hard ceiling, not a target - see MAX_LATENCY_SECONDS)
const PRIME_SECONDS = 0.15; // wait for this much buffered audio before unmuting playback
// The server and the browser's sound card are independent clocks - even a tiny relative drift
// between them accumulates linearly over a long session (observed: buffered-ahead latency
// crept up to PLAYBACK_RING_SECONDS, i.e. the ring's hard capacity, after running overnight).
// Left uncorrected, playback latency only ever grows. Actively trimming back to this target
// whenever it's exceeded (see handlePcmFrame) keeps steady-state latency near real-time
// indefinitely instead of slowly drifting toward - and getting stuck at - the ring's capacity.
// Deliberately close to PRIME_SECONDS (not a generous multiple of it): a low-latency scanner
// should stay as close to real-time as normal jitter allows, not just "bounded eventually" -
// a first attempt at 0.5s technically stopped the drift (verified: holds flat there
// indefinitely instead of continuing to 2s) but still let latency roughly double from the
// ~0.2s baseline before correcting, which is a real, audible regression on its own.
const MAX_LATENCY_SECONDS = 0.25;

class PlaybackRingBuffer {
  constructor(capacitySamples) {
    this.buf = new Float32Array(capacitySamples);
    this.capacity = capacitySamples;
    this.head = 0; // next write index
    this.tail = 0; // next read index
  }

  available() {
    return this.head >= this.tail ? (this.head - this.tail) : (this.capacity - this.tail + this.head);
  }

  write(samples) {
    for (let i = 0; i < samples.length; i++) {
      const next = (this.head + 1) % this.capacity;
      if (next === this.tail) {
        // Full - drop the oldest sample rather than blocking or growing unbounded. Should be
        // rare given the 2s capacity vs ~0.25s chunks; if it does happen it's a brief skip,
        // not a click (no discontinuity in what's actually played back).
        this.tail = (this.tail + 1) % this.capacity;
      }
      this.buf[this.head] = samples[i];
      this.head = next;
    }
  }

  // Fills dest with available samples, zero-filling any shortfall (silence on underrun, same
  // policy the server side uses - never desyncs playback timing, just goes quiet briefly).
  read(dest) {
    let n = 0;
    while (n < dest.length && this.tail !== this.head) {
      dest[n] = this.buf[this.tail];
      this.tail = (this.tail + 1) % this.capacity;
      n++;
    }
    for (; n < dest.length; n++) dest[n] = 0;
  }

  // Advances the read pointer by up to n samples without playing them - used to actively trim
  // excess buffered-ahead latency (see MAX_LATENCY_SECONDS/handlePcmFrame) rather than only
  // reacting once the ring is completely full.
  discard(n) {
    const toDrop = Math.min(n, this.available());
    this.tail = (this.tail + toDrop) % this.capacity;
  }
}

function audioWsUrl() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.hostname}:${AUDIO_WS_PORT}`;
}

function ensureAudioContext() {
  if (audioCtx) return;
  audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: AUDIO_SAMPLE_RATE });
  gainNode = audioCtx.createGain();
  gainNode.gain.value = getVolume();
  gainNode.connect(audioCtx.destination);

  playbackBuffer = new PlaybackRingBuffer(Math.round(AUDIO_SAMPLE_RATE * PLAYBACK_RING_SECONDS));
  playbackPrimed = false;

  processorNode = audioCtx.createScriptProcessor(PROCESSOR_BUFFER_SIZE, 1, 1);
  processorNode.onaudioprocess = (event) => {
    const out = event.outputBuffer.getChannelData(0);
    if (!playbackPrimed) {
      out.fill(0);
      if (playbackBuffer.available() >= AUDIO_SAMPLE_RATE * PRIME_SECONDS) playbackPrimed = true;
      return;
    }
    playbackBuffer.read(out);
  };
  processorNode.connect(gainNode);
}

function getVolume() {
  const el = document.getElementById("audioVolume");
  if (!el) return 0.8;
  const v = Number(el.value);
  return Number.isFinite(v) ? v : 0.8;
}

function handlePcmFrame(buf) {
  if (!playbackBuffer) return;

  const int16 = new Int16Array(buf);
  const f32 = new Float32Array(int16.length);
  for (let i = 0; i < int16.length; i++) f32[i] = int16[i] / 32768.0;
  playbackBuffer.write(f32);

  const maxSamples = AUDIO_SAMPLE_RATE * MAX_LATENCY_SECONDS;
  const excess = playbackBuffer.available() - maxSamples;
  if (excess > 0) playbackBuffer.discard(excess);

  lastPcmFrameAt = Date.now();
  refreshAudioIndicator();
}

function connectAudioWs() {
  const url = audioWsUrl();
  refreshAudioIndicator();
  audioWs = new WebSocket(url);
  audioWs.binaryType = "arraybuffer";

  audioWs.onopen = () => refreshAudioIndicator();
  audioWs.onclose = () => {
    refreshAudioIndicator();
    if (audioPlaying) setTimeout(connectAudioWs, 1000);
  };
  audioWs.onerror = () => refreshAudioIndicator();
  audioWs.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) return handlePcmFrame(ev.data);
    if (ev.data instanceof Blob) ev.data.arrayBuffer().then(handlePcmFrame);
  };
}

async function startAudio() {
  if (audioPlaying) return;
  audioPlaying = true;

  ensureAudioContext();
  // audioCtx.resume() can reject (browsers block audio output without a user gesture) - that's
  // expected on an auto-start attempt before the kiosk has been touched yet. Not fatal: it just
  // leaves the AudioContext suspended, which currentAudioState() reports as "blocked" so the
  // indicator tells the user to tap - and the document-level click handler in init() resumes it
  // on the very next tap anywhere on screen.
  try { await audioCtx.resume(); } catch {}

  // Reset playback state on (re)start - audioCtx/processorNode persist across stop/start
  // toggles, but stale buffered audio from before a stop shouldn't play immediately on resume.
  playbackBuffer = new PlaybackRingBuffer(Math.round(AUDIO_SAMPLE_RATE * PLAYBACK_RING_SECONDS));
  playbackPrimed = false;

  gainNode.gain.value = getVolume();
  document.getElementById("audioVolume")?.addEventListener("input", () => {
    if (gainNode) gainNode.gain.value = getVolume();
  });

  connectAudioWs();
  refreshAudioIndicator();
}

function stopAudio() {
  audioPlaying = false;
  if (audioWs) {
    try { audioWs.close(); } catch {}
    audioWs = null;
  }
  if (audioCtx) audioCtx.suspend().catch(() => {});
  refreshAudioIndicator();
}

function wireAudioControls() {
  const btn = document.getElementById("audioToggle");
  if (!btn) return;
  btn.addEventListener("click", async () => {
    if (!audioPlaying) {
      btn.textContent = "Stop";
      await startAudio();
    } else {
      btn.textContent = "Start";
      stopAudio();
    }
  });
}

(async function init() {
  ensureAudioControls();
  wireAudioControls();

  try { await loadSnapshot(); }
  catch (e) { log("snapshot failed: " + e); updateSelectedButtons(); }

  connectWS();
  setInterval(() => renderActiveList(), 1000);

  // Opt-in (see panel_ui/index.html) - a kiosk with no keyboard/mouse needs audio playing
  // without anyone having to find and tap a Start button first. The AudioContext itself may
  // still come up suspended (browser autoplay policy - see startAudio()'s comment); the
  // document-level click listener below resolves that on first touch instead.
  if (document.body.hasAttribute("data-autostart-audio")) {
    const btn = document.getElementById("audioToggle");
    if (btn) btn.textContent = "Stop";
    startAudio();
  }

  setInterval(refreshAudioIndicator, 500);
  document.addEventListener("click", () => {
    if (audioPlaying && audioCtx && audioCtx.state !== "running") {
      audioCtx.resume().catch(() => {});
    }
  });
})();
