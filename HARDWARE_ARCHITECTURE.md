# Handheld/Field Unit Hardware Architecture & Bill of Materials

Goal: a robust, technician-borrowable tool that clips directly onto a Vista
panel's 4-wire keypad (ECP) bus — no Envisalink or other intermediary
module required — for zone/config discovery today, config writes next, and
eventually broader datalogging (Polling Loop, zone-terminal I/O) as the
project grows. See `CONCEPT.md` for the full product concept, UX flow, and
roadmap this hardware serves; this doc stays focused on the physical build.

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
  capture, and keypad-address sniffing (scanning pulse-slot 3 — addresses
  16-23 per esphome-vistaECP's own pulse-allocation notes — for active
  keypads before the tool claims an address). Talks to the Pi over USB
  serial using the plain text protocol in `firmware/SERIAL_PROTOCOL.md`.
  This is a near-direct port of esphome-vistaECP's `VistaECP` library
  running outside ESPHome (its own README notes the library has no ESPHome
  dependency and can be called directly).
- **Raspberry Pi** — everything that isn't time-critical: the `vista_tool`
  Python backend (walk logic, safety checks, timeouts), the FastAPI web
  server (serving both the on-device touchscreen and any remote browser on
  the network — see `CONCEPT.md` networking section), and storage/logging.
  If the Pi hiccups, worst case is a slow UI update — never a corrupted bus
  frame.

This also means the RP2040 firmware is a genuinely separate, testable unit:
it can be bench-validated against a real panel with nothing but a serial
terminal, before the Pi software, touchscreen, or battery system are even
wired in — planned as the first real build/test milestone.

## Bill of materials (decisions marked; still-open items marked)

| Role | Part | Status |
|---|---|---|
| Compute | **Raspberry Pi 4, 8GB** | **Decided** — user has several on hand. Also settles wired Ethernet (built-in) and headroom for a larger touchscreen than originally scoped. |
| Bus coprocessor | Raspberry Pi Pico (RP2040) | **Decided for now.** Pinout follows esphome-vistaECP's documented RP2040 mapping. Treated as the first of potentially several interface modules (see "Modularity" below) — not expected to need reworking for the ECP-only milestone; a different/second MCU is an acceptable future refactor if the Pico can't keep up once Polling Loop or zone-terminal I/O modules are added, but that's a "cross that bridge later" concern, not a current blocker. |
| Display | **Undecided.** | Ruled out 12in (too tablet-scale for a Pi-driven build); ruled out reducing to a pocketable 3-5in. Still need to land on something in the 7-10.1in range pending how much needs to be on-screen at once (see `CONCEPT.md`). |
| Battery | **LiPo pouch cell** | **Decided.** Single-cell, sized once display/runtime targets are set. |
| Charge + power management | **USB-C charging circuit, with pass-through/overnight-charge support** | **Decided.** The device runs off battery in the field and stays on USB-C power (charging while running) for unattended overnight logging sessions — not powered from the panel's own AUX terminals. Needs a charge IC/board that supports simultaneous charge+discharge (e.g. TP4056-style boards do NOT reliably support this — look at USB-C PD trigger + a proper charge/power-path IC, or a PowerBoost-style board that explicitly supports it). Any needed voltage conversion (battery voltage to whatever the Pi 4 and Pico rails need) is part of this same subsystem. |
| Bus interface (Pico <-> panel) | **Ground-isolated** (optocouplers, per esphome-vistaECP's isolated schematic) | **Decided.** Chosen specifically because the device has its own independent power source (battery + USB-C charger) that can be at a different ground potential than the panel — the exact case esphome-vistaECP's isolated variant exists for. Accepts the signal-fidelity tradeoff their README notes for this variant; may need extra care in firmware/hardware tuning as a result. |
| Storage | **Industrial/endurance-rated microSD** | **Decided** — user has a good track record with these for continuous read/write workloads, covers the datalogging use case without needing an NVMe HAT. |
| Panel connection | 4-conductor cable + small screw terminal or keypad-style connector | Matches how a real alpha keypad taps the bus (red/black/yellow/green: +12V, GND, data-in, data-out). |
| Networking | Pi 4's built-in WiFi + Ethernet, software AP-mode fallback | See `CONCEPT.md` — WiFi client, WiFi hotspot (auto-fallback), and wired Ethernet all supported; no new hardware needed beyond what the Pi 4 already has. |
| Enclosure | **User-designed, 3D-printed** | Out of scope for this doc — sized around whatever display/battery/board stack gets finalized. |

## Modularity for future interface boards

The long-term roadmap (`CONCEPT.md`) adds Polling Loop bus monitoring (Vista
32/128/250 — a current-loop addressable-device protocol, electrically
distinct from ECP) and raw zone-terminal I/O (simple voltage/resistance
sensing, not a bus protocol at all). Neither is being built now — the
near-term goal is an ECP read/write utility — but the physical/electrical
design shouldn't paint itself into a corner: leave room (board space, a
spare USB port or header) for an additional interface module later rather
than assuming the ECP board is the only thing that will ever plug into the
Pi. Not a current blocker; revisit when those modules become real.

## Data flow

```
Vista panel keypad bus (4-wire ECP)
        │  (isolated opto interface)
        ▼
   RP2040 (Pico)  ── bit-bang ECP, emulate a virtual keypad address,
        │            scan pulse-slot 3 for in-use keypad addresses
        │  USB serial, text protocol (firmware/SERIAL_PROTOCOL.md)
        ▼
   Raspberry Pi 4 (8GB)  ── vista_tool Python backend, safety rules,
        │                    industrial microSD for logging
        │  WebSocket / HTTP, over WiFi (client or AP-mode hotspot) or Ethernet
        ├──────────────► On-device touchscreen (local kiosk view)
        └──────────────► Remote browser on a tech's laptop/phone (same UI,
                          same live scan/log data, concurrent sessions OK)
```

## Resolved items (previously open)

1. ~~Confirm the Pico's virtual keypad address won't collide with existing
   keypads/modules~~ — resolved procedurally, not technically: this follows
   standard technician SOP (identify the in-use address, disconnect the
   field keypad, take its address), which the tool actively supports via
   the address-sniffing onboarding flow in `CONCEPT.md`. Address changes
   are live/hot on this bus with no special handling needed.
2. ~~Decide isolation vs. non-isolation~~ — isolated, see BOM above.

## Still open

1. **Display size** (7-10.1in range) — pending UI density decision.
2. **Bench-validate the RP2040 firmware against a real Vista-20P** using a
   plain serial terminal before wiring in the Pi/UI/battery/enclosure —
   isolates bus timing bugs from application bugs, matching the "test
   methodology" lesson in the protocol notes (a live end-to-end run catches
   things a walk-through or a single capture won't). Next real milestone.
3. **Battery runtime budget** — sizing depends on final display choice and
   realistic day-of-use (multiple panels, idle time between jobs, possible
   overnight logging on USB-C power rather than battery).
4. **Concurrent-write safety at the protocol/firmware level** — the
   product decision is "concurrent sessions are fine" (multiple viewers OK,
   including while a scan/write is in progress), so this is about the
   RP2040/backend correctly serializing actual keystroke sends to the panel
   regardless of how many UI clients are connected, not about restricting
   who can watch or click.
