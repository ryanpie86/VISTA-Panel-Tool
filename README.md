# Vista Panel Tool

A field-technician tool for Honeywell/Ademco Vista-20P alarm panels:
zone discovery today (read zone types and names via the panel's own `*56`
and `*82` installer menus), with panel config read/write intended as later
scope. Designed to run on a handheld: a Raspberry Pi driving a touchscreen
web UI, paired with an RP2040 coprocessor that clips directly onto the
panel's keypad bus — no Envisalink module required in the field.

See `HARDWARE_ARCHITECTURE.md` (also saved to the project) for the handheld
BOM and wiring plan, and `firmware/SERIAL_PROTOCOL.md` for the Pi<->RP2040
contract. `VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md` (project doc) is the
source-of-truth for the panel protocol itself — this repo is that document
turned into runnable, transport-agnostic code.

## Status

- `vista_tool/zone_discovery.py` — the *56/*82 walk and parsing logic,
  ported from `envisalink_new`'s Home Assistant integration, transport-
  agnostic, safety rules intact. **Fully implemented and tested** (see
  `tests/`, including regression tests for the two real-hardware bugs the
  original implementation hit).
- `vista_tool/transports/tpi.py` — Envisalink TPI transport. Usable *today*
  against real hardware if you have an EVL3/EVL4 already installed — this
  is the fastest way to validate the walk logic before the handheld exists.
- `vista_tool/transports/rp2040_serial.py` — talks to the future RP2040
  coprocessor per `firmware/SERIAL_PROTOCOL.md`. **Not yet tested against
  real hardware** — the RP2040 firmware itself (bus bit-banging, adapted
  from `Dilbert66/esphome-vistaECP`'s ECP library) still needs to be built;
  this file is ready for it.
- `vista_tool/app.py` + `vista_tool/static/index.html` — minimal FastAPI
  backend and touchscreen-sized single-page UI. Runs a scan over a
  WebSocket with live per-zone progress.

## Running the UI against a real Envisalink today

```
pip install -r requirements.txt
export VISTA_TRANSPORT=tpi
export VISTA_EVL_HOST=192.168.1.100   # your EVL's IP
export VISTA_EVL_PASSWORD=...
uvicorn vista_tool.app:app --host 0.0.0.0 --port 8000
```

Then open the UI, enter the installer code, and start a scan. **Read the
safety notes in the protocol doc before running against a real panel** —
in particular, `*82` (zone names) has no read-only mode; every "read" is a
real re-save of the existing descriptor (harmless as long as the walk never
touches the character-entry keys mid-field, which it doesn't, but worth
understanding before running it).

## Running tests

```
python3 -m pytest tests/ -v
```

## Next steps

1. Build/flash the RP2040 firmware (port esphome-vistaECP's ECP class out
   of ESPHome, per its own README's note that the library works standalone;
   implement the line protocol in `firmware/SERIAL_PROTOCOL.md`).
2. Validate `RP2040SerialTransport` end-to-end against a real panel with a
   bench prototype before committing to the handheld enclosure/battery build.
3. Expand beyond zone discovery into broader config read (and eventually
   write) once the transport layer is proven, per the project's stated goal.
