// sdr-scanner web GUI (original layout) + browser audio playback
// Audio WS expects binary int16 little-endian mono PCM frames.

console.log("sdr-scanner app.js loaded (layout-preserving + audio + PIN elevate) v2026-01-13");

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

// ---------------------------
// Auth / Role state
// ---------------------------
let currentRole = "viewer"; // viewer | controller

function isController() {
  return currentRole === "controller";
}

function setRole(role) {
  currentRole = (role === "controller") ? "controller" : "viewer";
  updateControlLock();
  updateSelectedButtons();
  log(`role = ${currentRole}`);
}

// ---------------------------
// Audio config (override via window.* if needed)
// ---------------------------
const AUDIO_WS_PORT = (window.AUDIO_WS_PORT != null) ? Number(window.AUDIO_WS_PORT) : 8765;
const AUDIO_SAMPLE_RATE = (window.AUDIO_SAMPLE_RATE != null) ? Number(window.AUDIO_SAMPLE_RATE) : 16000;

// ---------------------------
// Logging helpers
// ---------------------------
function log(line) {
  const ts = new Date().toISOString();
  if (logEl) logEl.textContent = `[${ts}] ${line}\n` + logEl.textContent;
}

// ---------------------------
// Header audio + control elevation (inject into existing header)
// ---------------------------
function ensureAudioControls() {
  const header = document.querySelector("header");
  if (!header) return;

  // avoid duplicates on hot reload
  if (document.getElementById("audioControls")) return;

  const box = document.createElement("div");
  box.id = "audioControls";
  box.style.display = "flex";
  box.style.alignItems = "center";
  box.style.gap = "10px";
  box.style.flexWrap = "wrap";

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
  volLabel.style.opacity = "0.9";

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
  status.style.opacity = "0.9";

  box.appendChild(label);
  box.appendChild(btn);
  box.appendChild(volLabel);
  box.appendChild(vol);
  box.appendChild(status);

  // ---- Role / Control Elevation UI (same session) ----
  const sep = document.createElement("div");
  sep.textContent = "|";
  sep.style.opacity = "0.35";
  sep.style.margin = "0 4px";

  const roleLabel = document.createElement("div");
  roleLabel.id = "roleIndicator";
  roleLabel.textContent = "LISTEN ONLY";
  roleLabel.style.fontSize = "12px";
  roleLabel.style.fontWeight = "700";
  roleLabel.style.letterSpacing = "0.4px";
  roleLabel.style.color = "#f59e0b";

  const ctrlBtn = document.createElement("button");
  ctrlBtn.id = "controlToggle";
  ctrlBtn.textContent = "Enable Control";
  ctrlBtn.className = "btn";
  ctrlBtn.style.fontSize = "12px";

  ctrlBtn.onclick = () => {
    if (!isController()) {
      const pin = prompt("Enter control PIN:");
      if (!pin) return;
      send({ type: "AuthElevate", data: { pin } });
    } else {
      // drop back to listen-only without reconnect
      send({ type: "AuthDrop", data: {} });
    }
  };

  box.appendChild(sep);
  box.appendChild(roleLabel);
  box.appendChild(ctrlBtn);

  // Put audio controls to the right side of header (after connStatus)
  header.appendChild(box);
}

function setAudioStatus(text) {
  const el = document.getElementById("audioStatus");
  if (el) el.textContent = text;
}

// ---------------------------
// Control lock/unlock
// ---------------------------
function updateControlLock() {
  const locked = !isController();

  // Disable control buttons (even if a channel is selected)
  [
    btnHold,
    btnSolo,
    btnMute,
    btnForce,
    btnEnable,
    btnDisable1h
  ].forEach(b => {
    if (!b) return;
    b.disabled = locked || !selectedId;
    b.classList.toggle("disabled", b.disabled);
  });

  const meta = document.getElementById("roleIndicator");
  if (meta) {
    meta.textContent = locked ? "LISTEN ONLY" : "CONTROL ENABLED";
    meta.style.color = locked ? "#f59e0b" : "#10b981";
  }

  const tgl = document.getElementById("controlToggle");
  if (tgl) tgl.textContent = locked ? "Enable Control" : "Drop Control";
}

// ---------------------------
// Connection badge
// ---------------------------
function setConn(ok) {
  if (!connStatus) return;
  connStatus.textContent = ok ? "connected" : "disconnected";
  connStatus.style.background = ok ? "rgba(16,185,129,0.25)" : "rgba(239,68,68,0.25)";
}

// ---------------------------
// WS send
// ---------------------------
function send(msg) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify(msg));
}

// ---------------------------
// UI render helpers (original layout)
// ---------------------------
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
    if (aa !== bb) return bb - aa;
    return (sb.rssiOverThreshold || 0) - (sa.rssiOverThreshold || 0);
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
    const rssi = st.rssi_dBFS ?? st.rssi ?? "";
    const nf = st.noiseFloor_dBFS ?? st.noiseFloor ?? "";
    const vol = st.volume_dBFS ?? st.volume ?? "";
    meters.textContent = `RSSI: ${rssi}   NF: ${nf}   VOL: ${vol}`;

    div.appendChild(top);
    div.appendChild(meters);
    activeList.appendChild(div);
  }
}

function updateSelectedButtons() {
  if (!selMeta) return;

  if (!selectedId) {
    selMeta.textContent = "None";
    [btnHold, btnSolo, btnMute, btnForce, btnEnable, btnDisable1h].forEach(b => {
      if (!b) return;
      b.disabled = true;
      b.classList.remove("on");
    });
    updateControlLock();
    return;
  }

  const c = cfgById.get(selectedId) || {};
  selMeta.textContent = `${c.label ?? selectedId}  •  ${freqMHz(c.freq_hz)} MHz  •  ${c.mode ?? ""}`;

  btnHold?.classList.toggle("on", !!c.hold);
  btnMute?.classList.toggle("on", !!c.mute);
  btnForce?.classList.toggle("on", !!c.forceActive);
  btnSolo?.classList.toggle("on", c.solo === true);
  btnEnable?.classList.toggle("on", !!c.enabled);

  updateControlLock();
}

function selectChannel(id) {
  selectedId = id;
  renderActiveList();
  updateSelectedButtons();
}

// Button handlers (original)
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

// Snapshot load
async function loadSnapshot() {
  const r = await fetch("/api/state", { cache: "no-store" });
  const s = await r.json();

  for (const c of (s.channel_configs || [])) cfgById.set(String(c.id), c);
  for (const st of (s.channel_status || [])) {
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

  // Role updates from server (viewer/controller + errors)
  if (msg.type === "AuthInfo") {
    const role = msg.data?.role || "viewer";
    setRole(role);

    if (msg.data?.error) {
      alert(`Control denied: ${msg.data.error}`);
    }
    return;
  }

  // Optional: show forbidden errors cleanly
  if (msg.type === "Error" && msg.data?.error === "forbidden") {
    log(`forbidden: ${msg.data?.messageType || "control"} (listen-only)`);
    return;
  }

  if (msg.type === "Snapshot") {
    const d = msg.data || {};
    for (const c of (d.channel_configs || [])) cfgById.set(String(c.id), c);
    for (const st of (d.channel_status || [])) {
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

  if (msg.type === "FatalError") {
    log("FATAL: " + (msg.data?.error || "unknown"));
  }
}

function connectWS() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => {
    setConn(true);
    log("ws connected");
    // default to viewer until server says otherwise
    setRole("viewer");
  };
  ws.onclose = () => { setConn(false); log("ws disconnected - retrying"); setTimeout(connectWS, 1000); };
  ws.onerror = () => { setConn(false); };
  ws.onmessage = (ev) => {
    try { handleMsg(JSON.parse(ev.data)); }
    catch { /* ignore */ }
  };
}

// ---------------------------
// Browser Audio (int16 PCM over WS)
// ---------------------------
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
  if (nextPlayTime < now - 0.25) nextPlayTime = now + 0.02;

  src.start(nextPlayTime);
  nextPlayTime += audioBuf.duration;

  setAudioStatus(`playing (${Math.max(0, nextPlayTime - audioCtx.currentTime).toFixed(2)}s buffered)`);
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

// ---------------------------
// Init
// ---------------------------
(async function init() {
  ensureAudioControls();
  wireAudioControls();

  // default state is listen-only until elevated
  setRole("viewer");

  try { await loadSnapshot(); }
  catch (e) { log("snapshot failed: " + e); updateSelectedButtons(); }

  connectWS();
  setInterval(() => renderActiveList(), 1000);
})();
