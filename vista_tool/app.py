"""Minimal FastAPI backend for the field-technician web UI.

Serves a single-page touchscreen-friendly UI (static/index.html) and a
WebSocket that streams ScanProgress/ZoneResult events live, so the UI shows
per-zone progress instead of a spinner during a multi-minute scan (notes
section 10).

Run locally against a fake transport for UI development:
    uvicorn vista_tool.app:app --reload

Swap `build_transport()` to `RP2040SerialTransport(...)` once the handheld
exists, or `EnvisalinkTPITransport(...)` to validate against real hardware
today.
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import asdict
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .transports.tpi import EnvisalinkTPITransport
from .zone_discovery import ScanProgress, ZoneDiscoveryWalk, ZoneResult

STATIC_DIR = Path(__file__).parent / "static"

app = FastAPI(title="Vista Panel Tool")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def build_transport():
    """Transport selection is intentionally centralized here -- this is the
    one line that changes between "talking to an Envisalink for now" and
    "talking to the RP2040 handheld once it exists"."""
    mode = os.environ.get("VISTA_TRANSPORT", "tpi")
    if mode == "tpi":
        return EnvisalinkTPITransport(
            host=os.environ.get("VISTA_EVL_HOST", "192.168.1.100"),
            port=int(os.environ.get("VISTA_EVL_PORT", "4025")),
            password=os.environ.get("VISTA_EVL_PASSWORD", ""),
        )
    if mode == "rp2040":
        from .transports.rp2040_serial import RP2040SerialTransport

        return RP2040SerialTransport(device=os.environ.get("VISTA_SERIAL_DEVICE", "/dev/ttyACM0"))
    raise ValueError(f"Unknown VISTA_TRANSPORT: {mode!r}")


@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.websocket("/ws/scan")
async def scan_ws(websocket: WebSocket):
    """Client sends: {"installer_code": "...", "read_types": true,
    "read_names": false, "zones": [1,2,...] | null}. Server streams one JSON
    message per ScanProgress/ZoneResult until the scan finishes or errors."""
    await websocket.accept()
    try:
        params = await websocket.receive_json()
    except WebSocketDisconnect:
        return

    transport = build_transport()
    try:
        await transport.connect()
        walk = ZoneDiscoveryWalk(
            transport,
            installer_code=params["installer_code"],
            partition=params.get("partition", 1),
        )
        async for item in walk.run(
            zones=params.get("zones"),
            read_types=params.get("read_types", True),
            read_names=params.get("read_names", False),
        ):
            kind = "progress" if isinstance(item, ScanProgress) else "zone"
            try:
                await websocket.send_json({"kind": kind, **asdict(item)})
            except WebSocketDisconnect:
                break
    finally:
        await transport.close()
        try:
            await websocket.close()
        except RuntimeError:
            pass  # already closed by client disconnect
