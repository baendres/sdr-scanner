"""
Web-based GUI for sdr-scanner (replacement for wxPython UI).

- Runs Scanner in a background thread (same model as wxMainFrame.ScannerControlThread)
- Exposes:
    GET  /api/state     -> snapshot of channel configs + last known statuses
    WS   /ws            -> bidirectional message bus (UI->Scanner commands, Scanner->UI updates)
    GET  /              -> static web UI
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import queue
import threading
import time
import secrets  # <-- ADDED (for constant-time compare)
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Set

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Response
from fastapi.encoders import jsonable_encoder
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from .Scanner import Scanner


# Make sure we can see where this module is loaded from and which PID is running
print("WEB_GUI LOADED FROM:", __file__, "PID:", os.getpid(), flush=True)

log = logging.getLogger("uvicorn.error")


def ws_json(obj: Any) -> str:
    # Convert UUID/numpy/etc into JSON-safe primitives
    return json.dumps(jsonable_encoder(obj))


# ---------------------------
# ADDED: Listen-only + PIN elevation
# ---------------------------
CONTROL_PIN = os.getenv("SDRSCANNER_CONTROL_PIN", "").strip()

CONTROL_TYPES: Set[str] = {
    # These are the message types your current app.js can send that change scanner state:
    "ChannelHold",
    "ChannelMute",
    "ChannelForceActive",
    "ChannelSolo",
    "ChannelEnable",
    "ChannelDisableUntil",
    # (Add more here later if you introduce other control messages)
}


def is_control_message(mtype: str) -> bool:
    return mtype in CONTROL_TYPES


def pin_ok(pin: str) -> bool:
    """
    Validate the control PIN from env var SDRSCANNER_CONTROL_PIN.
    If SDRSCANNER_CONTROL_PIN is not set, elevation is disabled (safe).
    """
    if not CONTROL_PIN:
        return False
    p = (pin or "").strip()
    # allow 4-12 digits
    if not p.isdigit() or len(p) < 4 or len(p) > 12:
        return False
    return secrets.compare_digest(p, CONTROL_PIN)


@dataclass
class StateCache:
    channel_configs: Dict[str, Dict[str, Any]] = field(default_factory=dict)   # id -> json
    channel_status: Dict[str, Dict[str, Any]] = field(default_factory=dict)    # id -> status json
    scan_windows: Optional[Dict[str, Any]] = None


class ScannerWebBridge:
    """
    Bridges Scanner queue-based message API to asyncio/WebSockets.
    """
    def __init__(self, scanner: Scanner):
        self.scanner = scanner

        self.scanner_to_ui: "queue.Queue[Dict[str, Any]]" = queue.Queue()
        self.ui_to_scanner: "queue.Queue[Dict[str, Any]]" = queue.Queue()

        # Scanner expects normal queue.Queue input/output queues
        self.scanner.addInputQueue(self.ui_to_scanner)
        self.scanner.addOutputQueue(self.scanner_to_ui)

        self.state = StateCache()
        for cc in getattr(scanner, "channelConfigs", []):
            try:
                self.state.channel_configs[str(cc.id)] = cc.getJson()
            except Exception:
                pass

        self._ws_clients: Set[WebSocket] = set()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._broadcast_q: Optional["asyncio.Queue[Dict[str, Any]]"] = None

        self._scanner_thread: Optional[threading.Thread] = None
        self._drain_thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()

    def start(self):
        # Start scanner engine in a background thread (blocks inside runReceiverProcesses)
        self._scanner_thread = threading.Thread(
            target=self._scanner_runner,
            name="scanner-engine",
            daemon=True,
        )
        self._scanner_thread.start()

        # Start a drain thread to pull scanner_to_ui and push into asyncio queue
        self._drain_thread = threading.Thread(
            target=self._drain_runner,
            name="scanner-to-web-drain",
            daemon=True,
        )
        self._drain_thread.start()

    def stop(self):
        self._stop_evt.set()
        try:
            self.scanner.stop()
        except Exception:
            pass

    def attach_asyncio(self, loop: asyncio.AbstractEventLoop, broadcast_q: "asyncio.Queue[Dict[str, Any]]"):
        self._loop = loop
        self._broadcast_q = broadcast_q

    def _scanner_runner(self):
        try:
            self.scanner.runReceiverProcesses()
        except Exception as e:
            # best effort broadcast fatal error
            msg = {"type": "FatalError", "data": {"error": str(e)}}
            self._emit_to_asyncio(msg)

    def _emit_to_asyncio(self, msg: Dict[str, Any]):
        if self._loop and self._broadcast_q:
            self._loop.call_soon_threadsafe(self._broadcast_q.put_nowait, msg)

    def _drain_runner(self):
        while not self._stop_evt.is_set():
            try:
                msg = self.scanner_to_ui.get(timeout=0.25)
            except queue.Empty:
                continue

            # Update cache
            try:
                mtype = msg.get("type")
                if mtype == "ChannelConfig":
                    data = msg.get("data") or {}
                    cid = str(data.get("id"))
                    if cid:
                        self.state.channel_configs[cid] = data
                elif mtype == "ChannelStatus":
                    data = msg.get("data") or {}
                    cid = str(data.get("id"))
                    if cid:
                        self.state.channel_status[cid] = data
                elif mtype == "ScanWindowConfigsChanged":
                    self.state.scan_windows = {"changed": True, "ts": time.time()}
            except Exception:
                pass

            # Broadcast to clients
            self._emit_to_asyncio(msg)


def create_app(bridge: ScannerWebBridge, static_dir: str) -> FastAPI:
    app = FastAPI(title="sdr-scanner web gui")

    # Serve the UI assets in a cache-safe way for both /static/* and legacy /app.js /style.css
    app.mount("/static", StaticFiles(directory=static_dir), name="static")

    @app.get("/")
    def index():
        return FileResponse(f"{static_dir}/index.html")

    @app.get("/app.js")
    def app_js(response: Response):
        response.headers["Cache-Control"] = "no-store"
        return FileResponse(f"{static_dir}/app.js")

    @app.get("/style.css")
    def style_css(response: Response):
        response.headers["Cache-Control"] = "no-store"
        return FileResponse(f"{static_dir}/style.css")

    @app.get("/api/state")
    def api_state():
        payload = {
            "channel_configs": list(bridge.state.channel_configs.values()),
            "channel_status": list(bridge.state.channel_status.values()),
            "scan_windows": bridge.state.scan_windows,
            "ts": time.time(),
        }
        return JSONResponse(content=jsonable_encoder(payload))

    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket):
        await ws.accept()
        bridge._ws_clients.add(ws)
        log.info("WS client connected: %s", getattr(ws, "client", None))

        # ---------------------------
        # ADDED: per-session role state (default listen-only)
        # ---------------------------
        role = "viewer"  # "viewer" | "controller"

        # rate-limit PIN attempts (minimal, per-socket)
        elevate_failures = 0
        last_attempt_ts = 0.0
        lockout_until_ts = 0.0

        async def send_auth_info(error: Optional[str] = None):
            payload: Dict[str, Any] = {"type": "AuthInfo", "data": {"role": role}}
            if error:
                payload["data"]["error"] = error
            await ws.send_text(ws_json(payload))

        try:
            # ADDED: tell client role immediately
            await send_auth_info()

            # Send snapshot on connect
            await ws.send_text(ws_json({
                "type": "Snapshot",
                "data": {
                    "channel_configs": list(bridge.state.channel_configs.values()),
                    "channel_status": list(bridge.state.channel_status.values()),
                    "scan_windows": bridge.state.scan_windows,
                }
            }))

            while True:
                raw = await ws.receive_text()
                log.info("WS RAW: %s", raw)

                try:
                    msg = json.loads(raw)
                except Exception:
                    await ws.send_text(ws_json({"type": "Error", "data": {"error": "invalid json"}}))
                    continue

                # Forward directly into Scanner input queue (same format as wx UI)
                if isinstance(msg, dict) and "type" in msg:
                    mtype = str(msg.get("type") or "")

                    # ---------------------------
                    # ADDED: AuthElevate/AuthDrop (upgrade current session)
                    # ---------------------------
                    if mtype == "AuthElevate":
                        now = time.time()

                        if lockout_until_ts and now < lockout_until_ts:
                            await send_auth_info("locked_out")
                            continue

                        if now - last_attempt_ts < 2.0:
                            await ws.send_text(ws_json({"type": "Error", "data": {"error": "slow_down"}}))
                            continue
                        last_attempt_ts = now

                        data = msg.get("data") or {}
                        pin = ""
                        if isinstance(data, dict):
                            pin = str(data.get("pin") or "")

                        if pin_ok(pin):
                            role = "controller"
                            elevate_failures = 0
                            lockout_until_ts = 0.0
                            await send_auth_info()
                        else:
                            elevate_failures += 1
                            if elevate_failures >= 5:
                                lockout_until_ts = now + 60.0
                                await send_auth_info("too_many_attempts")
                            else:
                                await send_auth_info("invalid_pin")
                        continue

                    if mtype == "AuthDrop":
                        role = "viewer"
                        await send_auth_info()
                        continue

                    # ---------------------------
                    # ADDED: Permission gate (server-enforced)
                    # ---------------------------
                    if is_control_message(mtype) and role != "controller":
                        await ws.send_text(ws_json({
                            "type": "Error",
                            "data": {"error": "forbidden", "messageType": mtype}
                        }))
                        continue

                    log.info("WEB->SCANNER: %s", msg)
                    bridge.ui_to_scanner.put(msg)

                    # ACK back to browser so we can prove the server received it
                    await ws.send_text(ws_json({"type": "Ack", "data": {"ok": True, "echoType": msg.get("type")}}))
                else:
                    await ws.send_text(ws_json({"type": "Error", "data": {"error": "invalid message"}}))

        except WebSocketDisconnect:
            log.info("WS client disconnected: %s", getattr(ws, "client", None))
        except Exception as e:
            log.exception("WS handler error: %s", e)
            try:
                await ws.send_text(ws_json({"type": "Error", "data": {"error": str(e)}}))
            except Exception:
                pass
        finally:
            bridge._ws_clients.discard(ws)

    @app.on_event("startup")
    async def _startup():
        loop = asyncio.get_running_loop()
        broadcast_q: asyncio.Queue = asyncio.Queue()
        bridge.attach_asyncio(loop, broadcast_q)
        bridge.start()

        async def broadcaster():
            while True:
                msg = await broadcast_q.get()
                if not bridge._ws_clients:
                    continue
                payload = ws_json(msg)
                dead: List[WebSocket] = []
                for c in list(bridge._ws_clients):
                    try:
                        await c.send_text(payload)
                    except Exception:
                        dead.append(c)
                for c in dead:
                    bridge._ws_clients.discard(c)

        asyncio.create_task(broadcaster())
        log.info("Startup complete. Serving UI from: %s", static_dir)

    @app.on_event("shutdown")
    async def _shutdown():
        bridge.stop()
        log.info("Shutdown complete.")

    return app


def main():
    parser = argparse.ArgumentParser(prog="sdr-scanner-web")
    parser.add_argument("-c", "--config", default="sdrscan.yaml", help="Path to sdr-scanner YAML config")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", default=8080, type=int)
    args = parser.parse_args()

    scanner = Scanner.fromConfigFile(args.config)
    bridge = ScannerWebBridge(scanner)

    static_dir = os.path.join(os.path.dirname(__file__), "web_static")
    app = create_app(bridge, static_dir)

    import uvicorn
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
