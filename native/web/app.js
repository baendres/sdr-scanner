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
const ctcssInput = document.getElementById("ctcssInput");
const gainInput = document.getElementById("gainInput");
const btnSquelchApply = document.getElementById("btnSquelchApply");
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

function setAudioStatus(text) {
  const el = document.getElementById("audioStatus");
  if (el) el.textContent = text;
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
    meters.textContent = `RSSI: ${st.rssi ?? ""}   NF: ${st.noiseFloor ?? ""}   VOL: ${st.volume ?? ""}`;

    div.appendChild(top);
    div.appendChild(meters);
    activeList.appendChild(div);
  }
}

function updateSelectedButtons() {
  if (!selMeta) return;

  if (!selectedId) {
    selMeta.textContent = "None";
    [btnHold, btnSolo, btnMute, btnForce, btnEnable, btnDisable1h, btnSquelchApply, btnCtcssApply, btnCtcssClear, btnGainApply].forEach(b => {
      if (!b) return;
      b.disabled = true;
      b.classList.remove("on");
    });
    return;
  }

  const c = cfgById.get(selectedId) || {};
  selMeta.textContent = `${c.label ?? selectedId}  •  ${freqMHz(c.freq_hz)} MHz  •  ${c.mode ?? ""}`;

  [btnHold, btnSolo, btnMute, btnForce, btnEnable, btnDisable1h, btnSquelchApply, btnCtcssApply, btnCtcssClear, btnGainApply].forEach(b => {
    if (!b) return;
    b.disabled = false;
  });

  btnHold?.classList.toggle("on", !!c.hold);
  btnMute?.classList.toggle("on", !!c.mute);
  btnForce?.classList.toggle("on", !!c.forceActive);
  btnSolo?.classList.toggle("on", c.solo === true);
  btnEnable?.classList.toggle("on", !!c.enabled);

  if (squelchInput) squelchInput.value = c.squelchThreshold ?? "";
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

let audioWs = null;
let audioCtx = null;
let gainNode = null;
let nextPlayTime = 0;
let audioPlaying = false;

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
  nextPlayTime = audioCtx.currentTime + 0.05;
}

function getVolume() {
  const el = document.getElementById("audioVolume");
  if (!el) return 0.8;
  const v = Number(el.value);
  return Number.isFinite(v) ? v : 0.8;
}

const TARGET_LATENCY_SEC = 0.10;
const MAX_LATENCY_SEC = 0.30;
const STARTUP_PRIME_SEC = 0.06;

function handlePcmFrame(buf) {
  if (!audioCtx || !gainNode) return;

  const int16 = new Int16Array(buf);
  const f32 = new Float32Array(int16.length);
  for (let i = 0; i < int16.length; i++) f32[i] = int16[i] / 32768.0;

  const audioBuf = audioCtx.createBuffer(1, f32.length, AUDIO_SAMPLE_RATE);
  audioBuf.copyToChannel(f32, 0);

  const src = audioCtx.createBufferSource();
  src.buffer = audioBuf;
  src.connect(gainNode);

  const now = audioCtx.currentTime;
  if (nextPlayTime < now) nextPlayTime = now + STARTUP_PRIME_SEC;

  let latency = nextPlayTime - now;
  if (latency > MAX_LATENCY_SEC) {
    nextPlayTime = now + TARGET_LATENCY_SEC;
  }

  src.start(nextPlayTime);
  nextPlayTime += audioBuf.duration;

  setAudioStatus(`playing (${Math.max(0, nextPlayTime - now).toFixed(2)}s buffered)`);
}

function connectAudioWs() {
  const url = audioWsUrl();
  setAudioStatus("connecting...");
  audioWs = new WebSocket(url);
  audioWs.binaryType = "arraybuffer";

  audioWs.onopen = () => setAudioStatus("connected");
  audioWs.onclose = () => {
    setAudioStatus("disconnected");
    if (audioPlaying) setTimeout(connectAudioWs, 1000);
  };
  audioWs.onerror = () => setAudioStatus("error");
  audioWs.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) return handlePcmFrame(ev.data);
    if (ev.data instanceof Blob) ev.data.arrayBuffer().then(handlePcmFrame);
  };
}

async function startAudio() {
  if (audioPlaying) return;
  audioPlaying = true;

  ensureAudioContext();
  try { await audioCtx.resume(); } catch {}

  gainNode.gain.value = getVolume();
  document.getElementById("audioVolume")?.addEventListener("input", () => {
    if (gainNode) gainNode.gain.value = getVolume();
  });

  connectAudioWs();
}

function stopAudio() {
  audioPlaying = false;
  if (audioWs) {
    try { audioWs.close(); } catch {}
    audioWs = null;
  }
  if (audioCtx) audioCtx.suspend().catch(() => {});
  setAudioStatus("stopped");
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
})();
