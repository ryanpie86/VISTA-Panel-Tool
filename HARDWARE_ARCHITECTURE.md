# Handheld Hardware Architecture & Bill of Materials

Goal: a pocketable, battery-powered, touchscreen field tool that clips
directly onto a Vista panel's 4-wire keypad (ECP) bus — no Envisalink or
other intermediary module required — and runs the zone-discovery (and
eventually broader config) tooling in `vista_tool/`.

## Why two processors, not one

The ECP bus is a real-time, interrupt-driven pulse protocol (`Dilbert66/esphome-vistaECP`
bit-bangs it on bare ESP8266/ESP32/RP2040 GPIO with microsecond-scale
timing). A Raspberry Pi running Linux cannot guarantee that kind of timing
under its own scheduler — a dropped or jittered pulse on a live panel bus is
exactly the failure mode the panel's own inactivity/watchdog behavior
punishes (see the protocol notes, section 7, on the panel backing out of
programming mode with no warning). So the design splits the work the same
way `esphome-vistaECP` already validates:

- **RP2040 coprocessor** — owns the ECP bus in real time: bit-level pulse
  timing, address-slot arbitration, keystroke injection, alpha-display
  capture. Talks to the Pi over USB serial using the plain text protocol in
  `firmware/SERIAL_PROTOCOL.md`. This is a near-direct port of
  esphome-vistaECP's `VistaECP` library running outside ESPHome (its own
  README notes the library has no ESPHome dependency and can be called
  directly) — that repo's `VistaAlarm.yaml` already documents a stock RP2040
  Pico pinout (RX/yellow=GPIO21, TX/green=GPIO20, monitor=GPIO18) and both a
  preferred non-isolated resistor-divider circuit and a transistor-based
  alternative for the transmit side.
- **Raspberry Pi** — everything that isn't time-critical: the `vista_tool`
  Python backend (the *56/*82 walk logic, safety checks, timeouts), the
  FastAPI web server, and driving the touchscreen in kiosk mode. If the Pi
  hiccups, worst case is a slow UI update — never a corrupted bus frame.

This also means the RP2040 firmware is a genuinely separate, testable unit:
it can be bench-validated against a real panel with nothing but a serial
terminal, before the Pi software, touchscreen, or battery system are even
wired in.

## Bill of materials (starting point — swap for parts on hand)

| Role | Suggested part | Notes |
|---|---|---|
| Compute | Raspberry Pi Zero 2 W | Smallest Pi with enough CPU for Python + a WebSocket UI at 60fps-ish touch response. A Pi 4/5 works too if size/battery budget allows and USB-C PD charging is preferred over a dedicated charge IC. |
| Bus coprocessor | Raspberry Pi Pico (RP2040) or Pico W | Already has a documented pinout in esphome-vistaECP's own config (`VistaAlarm.yaml`) — least new ground to break in. Pico W's wireless is unused here (Pi handles networking) but costs little extra and keeps sourcing simple. |
| Display | 3.5"–5" DSI or SPI touchscreen (e.g. Waveshare/HyperPixel/official Pi touch display, resistive or capacitive) | Pick DSI over HDMI+USB-touch where possible — fewer cables, lower power. Resistive touch is more forgiving with gloves, common on field tools. |
| Battery | Single-cell 18650 Li-ion (2000–3500 mAh) or a slim LiPo pouch cell (2000+ mAh) | "One battery" per your spec — single-cell keeps the charge/protection circuit simple (no series balancing needed). |
| Charge + power management | Adafruit PowerBoost 1000C (charge + 5V boost + pass-through) or a dedicated USB-C PD trigger + charge IC (e.g. based on IP5306/MCP73871) | PowerBoost-style boards are the simplest path to "plug in USB-C, it charges and runs simultaneously" without designing analog charge circuitry from scratch. |
| Bus interface (Pico <-> panel) | Resistors + either optocouplers (4N35/TLP521, CTR ≥ 50) or transistors, per esphome-vistaECP's non-isolated "simple version" schematic | That project explicitly recommends the non-isolated simple version as best signal fidelity with minimal bus loading — ground-isolated version is not recommended (loads the bus more). |
| Panel connection | 4-conductor cable + small screw terminal or keypad-style RJ-style connector | Matches how a real alpha keypad taps the bus (red/black/yellow/green: +12V, GND, data-in, data-out). |
| Enclosure | 3D-printed or off-the-shelf handheld project box sized to the chosen screen + Pi + Pico + battery stack | Not specified further here — depends on the screen/battery chosen above. |

## Data flow

```
Vista panel keypad bus (4-wire ECP)
        │  (resistor-divider / opto or transistor interface)
        ▼
   RP2040 (Pico)  ── bit-bang ECP, emulate a virtual keypad address
        │  USB serial, text protocol (firmware/SERIAL_PROTOCOL.md)
        ▼
   Raspberry Pi  ── vista_tool Python backend (zone_discovery.py, safety rules)
        │  WebSocket, localhost
        ▼
   Touchscreen (Chromium kiosk mode or similar, showing vista_tool's web UI)
```

## Open items before committing to a build

1. **Confirm the Pico's virtual keypad address won't collide** with any
   existing real keypad/module addresses on the target panel (`*190`–`*196`
   assign partition keypad addresses — same caution esphome-vistaECP calls
   out for its own AUI/keypad address selection).
2. **Bench-validate the RP2040 firmware against a real Vista-20P** using a
   plain serial terminal before wiring in the Pi/UI/battery — isolates bus
   timing bugs from application bugs, matching the "test methodology"
   lesson in the protocol notes (a live end-to-end run catches things a
   walk-through or a single capture won't).
3. **Decide isolation vs. non-isolation** for the bus interface once you
   know whether the handheld's ground will ever be at a different potential
   than the panel's (e.g. if the handheld is also USB-charging from a
   grounded outlet while clipped to the panel) — esphome-vistaECP's
   ground-isolated variant exists specifically for that case, at some cost
   to signal fidelity.
4. **Battery runtime budget**: a full 64-zone dual walk (*56 + *82) takes
   several minutes per the protocol notes' timing data — worth sizing the
   battery against realistic day-of-use (multiple panels, idle UI time
   between jobs), not just active-scan current draw.
