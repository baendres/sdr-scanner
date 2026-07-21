// sdr-scanner settings page: CRUD for channels (live, no restart), receivers and outputs
// (database-only - see native/README.md, takes effect on the next sdrscan restart).

const logEl = document.getElementById("log");
function log(line) {
  const ts = new Date().toISOString();
  if (logEl) logEl.textContent = `[${ts}] ${line}\n` + logEl.textContent;
}

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  const r = await fetch(path, opts);
  let data = null;
  try { data = await r.json(); } catch { /* empty body */ }
  if (!r.ok) {
    const msg = (data && data.error) ? data.error : `HTTP ${r.status}`;
    throw new Error(msg);
  }
  return data;
}

function el(tag, attrs, children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "text") e.textContent = v;
    else if (k in e) e[k] = v;
    else e.setAttribute(k, v);
  }
  for (const c of children || []) e.appendChild(c);
  return e;
}

///
// Channels

function renderChannelRow(cc) {
  const label = el("input", { type: "text", value: cc.label });
  const freq = el("input", { type: "number", step: "0.001", value: (cc.freq_hz / 1e6).toString() });
  const mode = el("select", {}, ["FM", "NFM", "AM", "NOAA", "BFM_EAS"].map(m => el("option", { value: m, text: m, selected: m === cc.mode })));
  const squelch = el("input", { type: "number", step: "1", value: cc.squelchThreshold });
  const ctcss = el("input", { type: "number", step: "0.1", value: cc.ctcssToneHz != null ? cc.ctcssToneHz : "", placeholder: "off" });
  const gain = el("input", { type: "number", step: "1", value: cc.audioGain_dB });
  const dwell = el("input", { type: "number", step: "0.5", value: cc.dwellTime_s });
  const enabled = el("input", { type: "checkbox", checked: cc.enabled });

  const saveBtn = el("button", { className: "btn", text: "Save" });
  saveBtn.addEventListener("click", async () => {
    try {
      const freqHz = Math.round(parseFloat(freq.value) * 1e6);
      await api("PATCH", `/api/channels/${cc.id}`, {
        label: label.value,
        freq_hz: freqHz,
        mode: mode.value,
        squelchThreshold: parseFloat(squelch.value),
        ctcssToneHz: ctcss.value === "" ? null : parseFloat(ctcss.value),
        audioGain_dB: parseFloat(gain.value),
        dwellTime_s: parseFloat(dwell.value),
        enabled: enabled.checked,
      });
      log(`saved channel ${cc.id}`);
      await loadAll();
    } catch (e) { log(`save channel failed: ${e.message}`); }
  });

  const delBtn = el("button", { className: "btn danger", text: "Delete" });
  delBtn.addEventListener("click", async () => {
    if (!confirm(`Delete channel "${cc.label}"?`)) return;
    try {
      await api("DELETE", `/api/channels/${cc.id}`);
      log(`deleted channel ${cc.id}`);
      await loadAll();
    } catch (e) { log(`delete channel failed: ${e.message}`); }
  });

  return el("tr", {}, [
    el("td", {}, [label]),
    el("td", {}, [freq]),
    el("td", {}, [mode]),
    el("td", {}, [squelch]),
    el("td", {}, [ctcss]),
    el("td", {}, [gain]),
    el("td", {}, [dwell]),
    el("td", {}, [enabled]),
    el("td", { className: "row-btns" }, [saveBtn, delBtn]),
  ]);
}

async function addChannel() {
  const freqStr = document.getElementById("newChFreq").value;
  if (!freqStr) { log("add channel failed: frequency is required"); return; }
  const ctcssStr = document.getElementById("newChCtcss").value;
  try {
    await api("POST", "/api/channels", {
      label: document.getElementById("newChLabel").value || undefined,
      freq_hz: Math.round(parseFloat(freqStr) * 1e6),
      mode: document.getElementById("newChMode").value,
      squelchThreshold: parseFloat(document.getElementById("newChSquelch").value),
      ctcssToneHz: ctcssStr === "" ? null : parseFloat(ctcssStr),
      audioGain_dB: parseFloat(document.getElementById("newChGain").value),
      dwellTime_s: parseFloat(document.getElementById("newChDwell").value),
    });
    document.getElementById("newChLabel").value = "";
    document.getElementById("newChFreq").value = "";
    document.getElementById("newChCtcss").value = "";
    log("added channel");
    await loadAll();
  } catch (e) { log(`add channel failed: ${e.message}`); }
}

///
// Receivers

function renderReceiverRow(rc) {
  const type = el("select", {}, ["RTL-SDR", "SOAPY"].map(t => el("option", { value: t, text: t, selected: t === rc.type })));
  const deviceArg = el("input", { type: "text", value: rc.deviceArg || "" });
  const driver = el("input", { type: "text", value: rc.driver || "" });
  const gain = el("input", { type: "number", step: "1", value: rc.gain != null ? rc.gain : "", placeholder: "auto" });
  const enabled = el("input", { type: "checkbox", checked: rc.enabled });

  const saveBtn = el("button", { className: "btn", text: "Save" });
  saveBtn.addEventListener("click", async () => {
    try {
      await api("PATCH", `/api/receivers/${rc.id}`, {
        type: type.value,
        deviceArg: deviceArg.value === "" ? null : deviceArg.value,
        driver: driver.value === "" ? null : driver.value,
        gain: gain.value === "" ? null : parseFloat(gain.value),
        enabled: enabled.checked,
      });
      log(`saved receiver ${rc.id} (restart to apply)`);
      await loadAll();
    } catch (e) { log(`save receiver failed: ${e.message}`); }
  });

  const delBtn = el("button", { className: "btn danger", text: "Delete" });
  delBtn.addEventListener("click", async () => {
    if (!confirm(`Delete this receiver?`)) return;
    try {
      await api("DELETE", `/api/receivers/${rc.id}`);
      log(`deleted receiver ${rc.id} (restart to apply)`);
      await loadAll();
    } catch (e) { log(`delete receiver failed: ${e.message}`); }
  });

  return el("tr", {}, [
    el("td", {}, [type]),
    el("td", {}, [deviceArg]),
    el("td", {}, [driver]),
    el("td", {}, [gain]),
    el("td", {}, [enabled]),
    el("td", { className: "row-btns" }, [saveBtn, delBtn]),
  ]);
}

async function addReceiver() {
  const driver = document.getElementById("newRxDriver").value;
  const type = document.getElementById("newRxType").value;
  if (type === "SOAPY" && !driver) { log("add receiver failed: driver is required for SOAPY"); return; }
  const gainStr = document.getElementById("newRxGain").value;
  try {
    await api("POST", "/api/receivers", {
      type,
      deviceArg: document.getElementById("newRxDeviceArg").value || null,
      driver: driver || null,
      gain: gainStr === "" ? null : parseFloat(gainStr),
    });
    document.getElementById("newRxDeviceArg").value = "";
    document.getElementById("newRxDriver").value = "";
    document.getElementById("newRxGain").value = "";
    log("added receiver (restart to apply)");
    await loadAll();
  } catch (e) { log(`add receiver failed: ${e.message}`); }
}

///
// Outputs

// Returns { fields: HTMLElement, getConfig: () => object } for the type-specific config inputs.
function outputConfigEditor(type, config) {
  config = config || {};
  if (type === "udp") {
    const ip = el("input", { type: "text", value: config.serverIp || "127.0.0.1", placeholder: "server IP" });
    const port = el("input", { type: "number", value: config.serverPort != null ? config.serverPort : 12345, placeholder: "port" });
    return { fields: el("div", { className: "out-config-fields" }, [ip, port]), getConfig: () => ({ serverIp: ip.value, serverPort: parseInt(port.value, 10) }) };
  }
  if (type === "websocket") {
    const host = el("input", { type: "text", value: config.host || "0.0.0.0", placeholder: "bind host" });
    const port = el("input", { type: "number", value: config.port != null ? config.port : 8123, placeholder: "port" });
    return { fields: el("div", { className: "out-config-fields" }, [host, port]), getConfig: () => ({ host: host.value, port: parseInt(port.value, 10) }) };
  }
  if (type === "icecast") {
    const url = el("input", { type: "text", value: config.url || "", placeholder: "http://host:port/mount" });
    const password = el("input", { type: "password", value: config.password || "", placeholder: "source password" });
    return { fields: el("div", { className: "out-config-fields" }, [url, password]), getConfig: () => ({ url: url.value, password: password.value }) };
  }
  return { fields: el("span", { className: "hint", text: "(no config needed)" }), getConfig: () => ({}) };
}

function renderOutputRow(oc) {
  const type = el("select", {}, ["local", "udp", "websocket", "icecast"].map(t => el("option", { value: t, text: t, selected: t === oc.type })));
  const configCell = el("td", {});
  let editor = outputConfigEditor(oc.type, oc.config);
  configCell.appendChild(editor.fields);
  type.addEventListener("change", () => {
    editor = outputConfigEditor(type.value, {});
    configCell.replaceChildren(editor.fields);
  });

  const enabled = el("input", { type: "checkbox", checked: oc.enabled });

  const saveBtn = el("button", { className: "btn", text: "Save" });
  saveBtn.addEventListener("click", async () => {
    try {
      await api("PATCH", `/api/outputs/${oc.id}`, { type: type.value, config: editor.getConfig(), enabled: enabled.checked });
      log(`saved output ${oc.id} (restart to apply)`);
      await loadAll();
    } catch (e) { log(`save output failed: ${e.message}`); }
  });

  const delBtn = el("button", { className: "btn danger", text: "Delete" });
  delBtn.addEventListener("click", async () => {
    if (!confirm("Delete this output?")) return;
    try {
      await api("DELETE", `/api/outputs/${oc.id}`);
      log(`deleted output ${oc.id} (restart to apply)`);
      await loadAll();
    } catch (e) { log(`delete output failed: ${e.message}`); }
  });

  return el("tr", {}, [
    el("td", {}, [type]),
    configCell,
    el("td", {}, [enabled]),
    el("td", { className: "row-btns" }, [saveBtn, delBtn]),
  ]);
}

let newOutEditor = null;
function wireNewOutputTypeSelector() {
  const typeSel = document.getElementById("newOutType");
  const cell = document.getElementById("newOutConfigCell");
  const refresh = () => {
    newOutEditor = outputConfigEditor(typeSel.value, {});
    cell.replaceChildren(newOutEditor.fields);
  };
  typeSel.addEventListener("change", refresh);
  refresh();
}

async function addOutput() {
  try {
    await api("POST", "/api/outputs", {
      type: document.getElementById("newOutType").value,
      config: newOutEditor ? newOutEditor.getConfig() : {},
      enabled: document.getElementById("newOutEnabled").checked,
    });
    log("added output (restart to apply)");
    await loadAll();
  } catch (e) { log(`add output failed: ${e.message}`); }
}

///
// Restart

// Polls GET /api/state until it responds again (the new process is up after a docker-compose
// `restart: unless-stopped` cycle) or `timeoutMs` elapses. Requests fail with a network error
// while the old process has exited and the new one hasn't bound the port yet - that's the
// expected/normal state mid-restart, not a real failure, so it's just retried.
async function pollUntilBackOnline(timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    await new Promise(r => setTimeout(r, 1000));
    try {
      await api("GET", "/api/state");
      return true;
    } catch { /* still restarting */ }
  }
  return false;
}

async function restartNow() {
  if (!confirm("Restart sdrscan now to apply pending receiver/output changes? Scanning will briefly stop.")) return;
  const btn = document.getElementById("btnRestartNow");
  btn.disabled = true;
  btn.textContent = "Restarting...";
  try {
    await api("POST", "/api/restart");
  } catch (e) {
    log(`restart request failed: ${e.message}`);
    btn.disabled = false;
    btn.textContent = "Restart Now";
    return;
  }
  log("restart requested - waiting for sdrscan to come back online...");
  if (await pollUntilBackOnline()) {
    log("sdrscan is back online - reloading");
    window.location.reload();
  } else {
    log("sdrscan didn't come back within 30s - if it's not running under a restart policy " +
        "(e.g. docker compose's `restart: unless-stopped`), you'll need to start it manually");
    btn.disabled = false;
    btn.textContent = "Restart Now";
  }
}

///
// Load + wire up

async function loadAll() {
  const state = await api("GET", "/api/state");

  const channelsBody = document.getElementById("channelsBody");
  channelsBody.replaceChildren(...state.channels
    .slice()
    .sort((a, b) => a.freq_hz - b.freq_hz)
    .map(renderChannelRow));

  const receiversBody = document.getElementById("receiversBody");
  receiversBody.replaceChildren(...state.receivers.map(renderReceiverRow));

  const outputsBody = document.getElementById("outputsBody");
  outputsBody.replaceChildren(...state.outputs.map(renderOutputRow));

  document.getElementById("restartBanner").classList.toggle("show", !!state.restartRequired);
}

(async function init() {
  document.getElementById("btnAddChannel").addEventListener("click", addChannel);
  document.getElementById("btnAddReceiver").addEventListener("click", addReceiver);
  document.getElementById("btnAddOutput").addEventListener("click", addOutput);
  document.getElementById("btnRestartNow").addEventListener("click", restartNow);
  wireNewOutputTypeSelector();

  try {
    await loadAll();
    log("loaded");
  } catch (e) {
    log(`load failed: ${e.message}`);
  }
})();
