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
| Display | **GeeekPi 10.1in HDMI + USB-touch panel, 1280x800** | **Decided.** Not laptop-style — a small stand-up kiosk device with a kickstand, set down at the panel and worked via touch (or left running and monitored remotely). HDMI+USB (not DSI) at this size for wider vendor selection and resolution headroom; costs two cables instead of one DSI ribbon, and somewhat more power draw, both acceptable tradeoffs for the resolution gained. Single USB cable for touch input (not a hub), separate HDMI for video, separate external power adapter (not USB-bus-powered). |
| Battery | **LiPo pouch pack, capacity undecided** | **Config decided, capacity open.** 1S2P (two cells in parallel, single nominal voltage — no balance leads needed, matches the original "single-cell" simplicity goal). Capacity pending enclosure dimensions (user is developing the case and will supply real size constraints). Sizing reference from the runtime discussion: ~6000mAh gets you right at a bare 2-hour floor on a *fresh* pack at an estimated 9-11W system draw (Pi 4 + 10.1in touchscreen + Pico + conversion losses) — that floor erodes below 2 hours as the pack ages (LiPo cells typically lose 20-30% capacity over their service life). Assistant's recommendation, not yet acted on: target a ~4hr fresh runtime (~11,000-13,000mAh) for real margin, since this is a kickstand/kiosk device that sits at the panel rather than being carried, making the size/weight cost of a bigger pack low. Final call waits on case dimensions. Not needed during the development/testing phase — the build will run on isolated wall power (via the isolated USB-C/DC-DC charge path below) until hardware is confirmed working. |
| Charge + power management | **USB-C charging circuit, with pass-through/overnight-charge support, on an ISOLATED DC-DC/charge path** | **Decided (revised).** The device runs off battery in the field and stays on USB-C power (charging while running) for unattended overnight logging sessions — not powered from the panel's own AUX terminals. Needs a charge IC/board that supports simultaneous charge+discharge (TP4056-style boards do NOT reliably support this — look at USB-C PD trigger + a proper charge/power-path IC, or a PowerBoost-style board that explicitly supports it) AND provides galvanic isolation between the external USB-C input and the internal battery/Pi/Pico rails (e.g. an isolated DC-DC converter module on the charge path). This is where the ground-loop protection now lives — see "Isolation strategy" below. |
| Bus interface (Pico <-> panel) | **Non-isolated** (resistor-divider + opto/transistor, per esphome-vistaECP's "simple version" schematic — their recommended default) | **Decided (revised from ground-isolated).** Shares ground directly with the panel, same as a real physical keypad's wiring (4-wire, no isolation, always has been how keypads connect). Chosen for full signal fidelity with zero compromise — esphome-vistaECP's own README calls this the best-signal, most-recommended option and calls the ground-isolated variant "least recommended" for signal quality. See "Isolation strategy" below for why this is safe given where isolation now lives instead. |
| Storage | **Industrial/endurance-rated microSD** | **Decided** — user has a good track record with these for continuous read/write workloads, covers the datalogging use case without needing an NVMe HAT. |
| Panel connection | 4-conductor cable + small screw terminal or keypad-style connector | Matches how a real alpha keypad taps the bus (red/black/yellow/green: +12V, GND, data-in, data-out). |
| Networking | Pi 4's built-in WiFi + Ethernet, software AP-mode fallback | See `CONCEPT.md` — WiFi client, WiFi hotspot (auto-fallback), and wired Ethernet all supported; no new hardware needed beyond what the Pi 4 already has. |
| Enclosure | **User-designed, 3D-printed, with kickstand** | Out of scope for this doc — sized around the 10.1in display/battery/board stack above. Kiosk-style: sits upright at an angle at the panel, not held/carried like a laptop while in use. |

## Isolation strategy: isolate the power path, not the data path

Earlier revision of this doc put isolation on the ECP bus interface itself,
reasoning that the device's own independent power source (battery +
charger) could sit at a different ground potential than the panel. On
reflection that's solving the problem in the wrong place:

- **A real physical Vista keypad shares ground with the panel directly, with
  zero isolation, and that's fine** — the risk was never "touching the
  panel's ground," it's specifically having a *second*, independent
  connection to a *different* ground reference at the same time. A keypad
  never has that second connection; this device does, because of the
  USB-C charging path.
- Tying the device's ground to the panel's ground (non-isolated bus) is
  actually the fidelity-optimal choice — it's what esphome-vistaECP
  recommends by default, and it's how the real hardware already works.
- The actual ground-loop risk lives entirely in the charging path: if the
  device is charging from AC power that's referenced to a different earth
  ground than the panel's own AC-derived ground, bonding the device's
  ground to the panel's (via a non-isolated bus) could pull current through
  that charging connection. In practice this is a narrower risk than it
  sounds — most USB-C wall chargers are already internally isolated
  between mains and DC output (a standard safety-certification
  requirement), and a single building's outlets and its alarm panel are
  normally bonded to the same earth reference at the service panel anyway.
  But relying on "the charger a tech happens to grab is probably isolated"
  is a field-dependent assumption, not a guarantee.
- So: **isolate the power input instead.** An isolated DC-DC converter or
  isolated USB-C charge module between the external power connector and
  the internal battery/Pi/Pico rails closes the ground-loop risk
  regardless of what charging source gets used in the field (wall brick,
  laptop USB port, car adapter, whatever), while leaving the ECP bus
  interface fully non-isolated for full signal fidelity, always. Best of
  both, rather than a compromise between them.

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

## Wireless (RF) receiver visibility

Because the bus interface is a non-isolated tap on the same 4-wire keypad
bus a wireless receiver module (e.g. 5881ENL) broadcasts onto, this
hardware is electrically positioned to see raw RF receiver sentences the
same way AlarmDecoder (AD2) does — a superset of what an Envisalink
EVL3/EVL4 can ever expose over TPI, since the EVL4 only relays the panel's
own already-decided reporting. That RF-sentence decode is not implemented
by the current firmware plan (esphome-vistaECP's `VistaECP` class targets
keystroke injection and alpha-display capture) — it would need to be added
explicitly. See CONCEPT.md's "Wireless (RF) zone visibility" section for
the testing that established this and the open-thread tracking it.

## Data flow

```
Vista panel keypad bus (4-wire ECP)
        │  (non-isolated resistor-divider + opto/transistor interface,
        │   shares ground with panel — same as a real keypad)
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

   USB-C external power ── [ISOLATED DC-DC / charge module] ── battery + Pi/Pico rails
        (ground-loop protection lives here now, not on the bus interface)
```

## Resolved items (previously open)

1. ~~Confirm the Pico's virtual keypad address won't collide with existing
   keypads/modules~~ — resolved procedurally, not technically: this follows
   standard technician SOP (identify the in-use address, disconnect the
   field keypad, take its address), which the tool actively supports via
   the address-sniffing onboarding flow in `CONCEPT.md`. Address changes
   are live/hot on this bus with no special handling needed.
2. ~~Decide isolation vs. non-isolation~~ — resolved as non-isolated bus +
   isolated power path, not a fully isolated bus interface. See "Isolation
   strategy" section above for the reasoning.
3. ~~Display size~~ — 10.1in HDMI+USB-touch, kiosk/kickstand form factor, see
   BOM above.
4. ~~Display model~~ — GeeekPi 10.1in 1280x800, see BOM above.

## Still open

1. **Bench-validate the RP2040 firmware against a real Vista-20P** using a
   plain serial terminal before wiring in the Pi/UI/battery/enclosure —
   isolates bus timing bugs from application bugs, matching the "test
   methodology" lesson in the protocol notes. Next real milestone.

   **Update (scope captures, pre-RP2040):** First oscilloscope readings
   taken directly off the live bus (existing keypad still attached).
   Findings:
   - Idle-high ~13.0-13.2V, active-low ~0.49-0.69V on the Green (RX) line —
     confirms the resistor-divider + NPN-transistor interface (non-isolated,
     per the BOM decision above) is workable as designed.
   - Bit timing is much slower than the "microsecond-scale" figure this doc
     inherited from esphome-vistaECP's general characterization: measured
     bit cells ≈3.06ms (326Hz), grouped into byte/frame bursts ≈53ms apart
     (18Hz), full poll transaction repeating ≈663ms (1.5Hz). This gives the
     RP2040 considerably more timing slack than assumed — worth confirming
     with a couple more captures, but a good sign for firmware margin.
   - Divider math confirmed against real levels: 13.0V × (3.3k/13.3k) ≈
     3.2V, matching the interface circuit's own design target.
   - **Two follow-ups before finalizing R1/R2 values:** (a) the Yellow
     (TX-from-panel) line — the one actually feeding the RP2040 GPIO — still
     needs a clean full-scale capture; the only capture taken so far was
     misconfigured at 100mV/div on a 13V line and came out clipped. (b) At
     worst-case AUX spec (13.8-14V vs. the ~13V measured on this bench),
     the divider output approaches ~3.4-3.5V against the RP2040's ~3.6V
     GPIO absolute max — thin margin for a tool that'll see other panels
     in the field. Recommend either tightening the divider ratio (e.g. R2
     → 2.2k) or adding a small clamp diode to 3.3V for insurance.
   - **Draft interface schematic has a pin conflict**: GPIO_26 was labeled
     as both the Yellow-line input and the driver for the Green-line
     transistor's base resistor. Needs two distinct GPIOs — GPIO_26 stays
     as the Yellow input, base-drive moves to GPIO_27 (or a third pin, if
     read-back of the RP2040's own drive on the Green line is wanted for
     collision/arbitration sensing).
2. **Battery capacity** — deliberately left undecided, and not needed
   during the development/testing phase — the build will run on isolated
   wall power (via the isolated USB-C/DC-DC charge path already in the
   BOM) until hardware is confirmed working. Config (1S2P) is settled;
   final mAh waits on real enclosure dimensions once the user's case design
   is further along. Minimum requirement once it matters: 2 hours (average
   service call duration) on battery alone. See BOM row above for the
   runtime math and the assistant's margin recommendation.
3. **Concurrent-write safety at the protocol/firmware level** — the
   product decision is "concurrent sessions are fine" (multiple viewers OK,
   including while a scan/write is in progress), so this is about the
   RP2040/backend correctly serializing actual keystroke sends to the panel
   regardless of how many UI clients are connected, not about restricting
   who can watch or click.
