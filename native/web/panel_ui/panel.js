// panel_ui-only: the Channels flyout (browse/select from every configured channel, not just
// recently-active ones) and the Config flyout (squelch/CTCSS/gain for whichever channel is
// selected). Deliberately separate from app.js - that file is shared with the main/settings
// pages and has no business knowing about this page's layout. Reads app.js's top-level
// cfgById/statusById/selectedId/selectChannel directly - classic (non-module) <script> tags on
// the same page share one global scope, so those are visible here without any glue code.

(function () {
  const backdrop = document.getElementById("flyoutBackdrop");

  function wireFlyout(flyoutId, openBtnId, closeBtnId, onOpen) {
    const flyout = document.getElementById(flyoutId);
    const openBtn = document.getElementById(openBtnId);
    const closeBtn = document.getElementById(closeBtnId);
    if (!flyout || !openBtn || !closeBtn || !backdrop) return;

    openBtn.addEventListener("click", () => {
      if (onOpen) onOpen();
      flyout.classList.add("open");
      backdrop.classList.add("open");
    });
    closeBtn.addEventListener("click", () => close(flyout));
    backdrop.addEventListener("click", () => close(flyout));
  }

  function close(flyout) {
    flyout.classList.remove("open");
    backdrop.classList.remove("open");
  }

  function badge(text, cls) {
    const b = document.createElement("span");
    b.className = "badge " + (cls || "");
    b.textContent = text;
    return b;
  }

  // Full channel list (cfgById), not just ones with a recent status update (statusById) - a
  // channel that's disabled or has simply never triggered still needs to be reachable to
  // configure. Mirrors renderActiveList()'s strip markup/classes so it picks up the same
  // styling for free.
  function renderChannelsFlyoutList() {
    const listEl = document.getElementById("channelsFlyoutList");
    if (!listEl || typeof cfgById === "undefined") return;
    listEl.innerHTML = "";

    const rows = Array.from(cfgById.values()).sort((a, b) => (a.freq_hz || 0) - (b.freq_hz || 0));
    for (const c of rows) {
      const id = String(c.id);
      const st = (typeof statusById !== "undefined" && statusById.get(id)) || {};

      const div = document.createElement("div");
      div.className = "strip" + (typeof selectedId !== "undefined" && selectedId === id ? " selected" : "");
      div.addEventListener("click", () => {
        if (typeof selectChannel === "function") selectChannel(id);
        close(document.getElementById("channelsFlyout"));
      });

      const top = document.createElement("div");
      top.className = "top";
      const left = document.createElement("div");
      const freqMHz = c.freq_hz != null ? (c.freq_hz / 1e6).toFixed(3) : "";
      left.innerHTML = `<div class="label">${c.label ?? id}</div><div class="freq">${freqMHz} MHz</div>`;

      const badges = document.createElement("div");
      badges.className = "badges";
      if (!c.enabled) {
        badges.appendChild(badge("DISABLED", "mute"));
      } else {
        badges.appendChild(badge(st.status || "IDLE", st.status === "ACTIVE" ? "active" : ""));
      }

      top.appendChild(left);
      top.appendChild(badges);
      div.appendChild(top);
      listEl.appendChild(div);
    }
  }

  wireFlyout("channelsFlyout", "btnChannelsFlyout", "btnCloseChannelsFlyout", renderChannelsFlyoutList);
  wireFlyout("configFlyout", "btnConfigFlyout", "btnCloseFlyout");

  document.getElementById("btnReloadPage")?.addEventListener("click", () => location.reload());

  // TEMPORARY - diagnosing a real-hardware sizing mismatch (xrandr reports 720x480, but the
  // panel's own config.txt configures a 480x320 framebuffer - something in between, likely
  // fbcp-style mirroring given hdmi_ignore_hotplug/display_auto_detect are both off, may be
  // scaling one to the other). window.innerWidth/innerHeight is what Chromium actually uses as
  // its CSS pixel viewport regardless of what's happening at the X11/framebuffer level below it
  // - the one number that actually matters for sizing this page. Remove once confirmed.
  const dbg = document.createElement("div");
  dbg.style.cssText = "position:fixed; bottom:0; left:0; z-index:999; background:#ef4444; " +
    "color:#fff; font-size:20px; font-weight:700; padding:4px 10px;";
  dbg.textContent = `viewport: ${window.innerWidth}x${window.innerHeight}`;
  document.body.appendChild(dbg);
})();
