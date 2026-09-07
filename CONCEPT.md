# VISTA Panel Tool — Product Concept

This is the running concept/requirements doc for the project, capturing
decisions made in conversation before any of this is built. See also:
`VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md` (the ECP protocol itself),
`HARDWARE_ARCHITECTURE.md` (the physical build this doc drives), and
`firmware/SERIAL_PROTOCOL.md` (the Pi<->RP2040 contract).

## What this is

A robust field tool for Honeywell/Ademco Vista alarm panels, built and
owned by the user, used personally and lent out to their technicians. It
serves two roles, not one:

1. **Portable programming tool** — carried to a panel, connected to the
   ECP bus, used to read (and eventually write) configuration: zone types,
   zone names, and beyond.
2. **Permanent installation / Home Automation Mode** — can be left
   connected and powered at a panel indefinitely, not just for overnight
   troubleshooting. Once ECP read-mode is solid on the RP2040, this is an
   accelerated (not deferred) feature: a UI-selectable mode where the same
   hardware that reads zones today doubles as a full-time replacement for
   an Envisalink module, publishing live panel state and accepting
   commands for a Home Assistant plugin (or a debug/test session) to
   connect to — see "Live monitoring / Home Automation Mode" below.

Both roles run on the same hardware and the same software — there's no
separate "logger mode" device, just the same tool used differently.

## Hardware direction (summary — full detail in HARDWARE_ARCHITECTURE.md)

- Raspberry Pi 4 (8GB) as an interim/dev compute board — the Pi Zero 2 W is
  the actual target board but is unobtainable during the ongoing 2026
  shortage (other small boards were evaluated and rejected) — paired with
  a Waveshare RP2040-Zero coprocessor (replacing the Pico) for real-time
  ECP bus timing, linked over USB-serial (a UART-over-GPIO-header plan was
  tried and reverted once remote firmware flashing from the Pi came into
  scope — see HARDWARE_ARCHITECTURE.md "RP2040-Zero <-> Pi interconnect").
  Android-tablet and fully-custom-tablet alternatives were both considered
  and rejected early on: Android would require sideloading/rooting to get
  serial access to custom hardware, which the user doesn't want; a fully
  custom 12in build was more hardware R&D than warranted once a Pi-based
  approach was back on the table. Full detail in HARDWARE_ARCHITECTURE.md.
- Non-isolated ECP bus interface (shares ground with the panel, same as a
  real physical keypad, for full signal fidelity) — isolation instead lives
  on the power path (see below), which is where the actual ground-loop
  risk from USB-C charging sits. Full reasoning in
  HARDWARE_ARCHITECTURE.md's "Isolation strategy" section.
- LiPo battery + USB-C charging circuit on an isolated DC-DC/charge path,
  supporting charging while running, for both portable use and overnight
  stationary logging.
- Industrial/endurance-rated microSD for storage.
- **No physical display** — the device is fully headless, interacted with
  exclusively through a browser (see "Networking" below). The GeeekPi
  10.1in touchscreen originally scoped for an on-device kiosk view was
  dropped along with wired Ethernet as part of the same headless/WiFi-only
  simplification.
- Enclosure: user-designed and 3D-printed; no longer needs to accommodate
  or prop up a display now that the device is headless — exact form factor
  still the user's call, not a software/electrical concern for this doc.

## Networking

The device is WiFi-only now (wired Ethernet was dropped along with the
physical display — see "Hardware direction" above) and fully headless, so
the network connection is also the only way in: there's no local
touchscreen fallback if WiFi setup goes wrong.

- **Boots into AP mode by default**, broadcasting its own hotspot.
- A tech joins that hotspot and submits WiFi credentials for the site
  network through the web UI.
- The device attempts a **STA (client) connection** to that network. If it
  hasn't connected within **5 minutes**, it gives up and falls back to AP
  mode so the tech is never locked out.
- **Every reboot clears stored WiFi credentials and returns to AP mode** —
  deliberately, not just on a failed connection attempt. This is a
  simplicity/troubleshooting tradeoff (a tech always knows "power-cycle it
  and it's back on its own hotspot," no stale-credential debugging) rather
  than an attempt at persistent remembered-network convenience.
- Must stay **2.4GHz-only** in the AP/STA config — the Pi Zero 2 W target
  board has no 5GHz radio, even though the Pi 4 dev board does (see
  HARDWARE_ARCHITECTURE.md "Compute board: Pi 4 now, Zero 2 W target").
- **Still open:** the actual AP/STA switching implementation (hostapd +
  wpa_supplicant + a watchdog script, vs. NetworkManager, vs. RaspAP) —
  deferred, not blocking near-term work. See "Open threads" below.

The web UI is identical regardless of client — every browser (a tech's
laptop or phone) is just a client of the same local FastAPI server, over
whichever mode (AP or STA) is currently active. **Concurrent sessions are
explicitly fine** — multiple viewers can be connected at once. This does
NOT mean multiple uncoordinated keystroke streams reach the panel — the
backend/firmware layer is responsible for serializing actual writes to the
bus regardless of how many browsers are watching or interacting; that's an
implementation detail below the product-level "concurrent viewing is fine"
decision.

## Onboarding / keypad-address flow

Before any menu loads, first-run (or every-run, TBD) sequence:

1. **Select panel model** from a list. Starts with just Vista-20P (what's
   validated so far); Vista-10P/15P and Vista-128BPT come next (see
   roadmap below). This determines default address suggestions, valid
   zone ranges, and other model-specific behavior.
2. **Scan for active keypad addresses** — RP2040 watches pulse-slot 3 (the
   keypad-address pulse per esphome-vistaECP's documented bus-pulse
   allocation) for up to 60 seconds, surfacing detected addresses to the
   UI as they're found (not just at the end).
3. **Present results + manual entry**, always available regardless of scan
   results. Manual entry is pre-populated with model-appropriate
   suggestions: address 16 for the 10/15/20 series (a fixed address on
   those panels that can't be reassigned), address 00 for the 32/128/250
   series (same idea, different fixed address).
4. **Tech selects/enters an address.**
5. **"Disconnect field keypad #NN and press OK to continue"** — the tool
   is not live on the bus yet at this point.
6. **On confirmation, the RP2040 immediately starts responding as that
   address** — no reboot or re-entry sequence needed. Address changes on
   this bus are live/hot; this is standard technician practice, not a
   novel or risky operation (a tech can even swap a live keypad's address
   while using it, with no ill effect, as long as the new address is
   active in the panel's own configuration).

This is deliberately built to match existing technician SOP (identify the
in-use address, pull the field keypad, take its address) rather than
inventing a new workflow — the sniffing feature just automates the
"identify" step instead of requiring the tech to already know it.

## Software scope: read/write

**Read is done.** `vista_tool/zone_discovery.py` implements the `*56`/`*82`
walk end-to-end, ported from the original Home Assistant integration, with
its safety rules intact (see `VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md`).

**Write is deferred — an add-on feature, not near-term work.** The
priority order is: get the hardware (RP2040) built and confirmed
operating, get ECP *reading* working end-to-end on that hardware, and
build live monitoring / Home Automation Mode (see "Live monitoring / Home
Automation Mode" below — accelerated ahead of write-mode since it needs no
new protocol research) before write-mode is touched at all. Write is still
understood as the eventual mirror-image operation — navigating to the same
menus/fields but
entering new values instead of just reading, with a symmetric verify
pattern (re-read after any write using the existing read mechanism, rather
than trusting the write blind) — but none of that starts until hardware
bring-up and read-mode are solid. The exact keystroke sequences for
entering edit mode and committing new values still need to be documented
when the time comes — this is real new protocol knowledge, not something
already captured in the existing notes, and will need the same
real-hardware care the original read-side work required (see the "six
real-hardware corrections" in the protocol notes as a cautionary example of
how easy this is to get subtly wrong).

**Reading is not an end in itself — it exists to feed two downstream uses,
both committed scope:**

1. **Edit and write back** — the write-mode work described above: load a
   saved scan, let the tech change zone types/names, push the changes to
   the panel, verify by re-reading.
2. **Report generation (CSV/PDF)** — export a saved scan as a
   technician-facing report, independent of write-mode. A tech may only
   ever want documentation of what a panel is currently programmed with,
   never touching write-mode at all.

Neither downstream use is fully built yet (report rendering — CSV to start,
PDF later — is itself deferred, same as write-mode), but the UI already
treats a completed scan as a saved artifact rather than a one-shot display:
the web UI's "Save" action on the zone-discovery screen exports the scan to
CSV today, which doubles as both a first-cut report and the natural input
format for the write-mode editor once it exists. The UI is structured as a
home screen with multiple tool entries (Zone Discovery today; Write
Configuration and Reports as visible but not-yet-built placeholders) rather
than a single-purpose scanning page, since this tool's mandate has always
been broader than zone discovery alone.

Once read/write is solid for ECP, the roadmap continues into general I/O
(zone terminal voltage/resistance sensing — not a bus protocol) and Polling
Loop bus support (Vista 32/128/250 commercial panels — a current-loop
addressable-device protocol, electrically distinct from ECP). Both are
explicitly deferred until the ECP read/write utility is solid; the hardware
is meant to leave room for these as future add-on interface modules rather
than being redesigned for them later (see HARDWARE_ARCHITECTURE.md
"Modularity" section).

## Live monitoring / Home Automation Mode (accelerated)

**Committed scope, accelerated ahead of write-mode** — this needs no new
protocol knowledge and no new hardware beyond the RP2040 already planned
for read-mode, so it doesn't have to wait behind write-mode's harder,
undocumented keystroke-sequence work. Priority order is now:

1. Get ECP read-mode solid on real RP2040 hardware (already the #1
   priority, unchanged).
2. **Live monitoring / Home Automation Mode** (this section) —
   straightforward extension of what read-mode already builds.
3. Write-mode — still deferred behind both of the above; still needs new
   protocol research (see "Software scope: read/write").

**The UI feature: Home Automation Mode.** A mode a tech switches on from
the web UI (alongside Zone Discovery / Write Configuration / Reports on
the Tools menu) that puts the device into a permanently-connected state
instead of one-shot scans: it holds the keypad address continuously and
exposes live panel state + control over IP for a companion consumer to
connect to. Two concrete uses, both served by the same mode:

1. **A Home Assistant integration** — a separate plugin/custom component
   (its own codebase, not part of this repo, likely following the pattern
   `envisalink_new` already established for Envisalink) connects over IP,
   receives live state, and sends commands, registering with HA as an
   "Alarm Panel" integration entity the same way an Envisalink-backed
   integration does today.
2. **Debug/test sessions** — leaving the device connected to a bench panel
   during development, watching live bus activity without re-running a
   scan. This alone is enough reason to build this ahead of write-mode:
   it's useful the moment ECP read-mode exists, before any HA plugin does.

**What "more granular than Envisalink" means here:** scope is ECP-bus data
only — no new sensing hardware, no zone-terminal voltage/resistance taps
(that idea remains the separate, still-deferred "general I/O" item below,
not pulled forward by this decision). The granularity gain is about how
much of the *bus* gets exposed, not additional physical inputs. An
Envisalink's TPI interface sits on the bus as a virtual keypad and
publishes a filtered subset of what it sees — zone/partition status bits
and commands shaped by its own schema. This tool's RP2040 sits on the bus
the exact same way (same virtual-keypad mechanism `zone_discovery.py`
already uses for the *56/*82 walk), so it can publish more of what's
actually on the wire: the raw keypad alpha-display stream and per-zone/
per-partition state changes as they happen, not just whatever subset TPI's
schema chose to model. (This is the wired-bus counterpart to "Wireless
(RF) zone visibility" below, which covers the separate, still-unbuilt
wireless-receiver-decode angle on "more granular than Envisalink.")

**How it's built:** reuses the existing transport abstraction
(`PanelTransport`, `KeypadUpdate`, `wait_for_display`/`last_update`) as-is
— no new hardware-facing protocol work. Where `zone_discovery.py` drives
the walk once per scan, Home Automation Mode is the same primitives run
continuously: watch the keypad display stream, parse it into
zone/partition state changes and expose them (and accept arm/disarm and
other keystroke commands) as a standing service instead of a one-shot
scan. Concrete wire format is still open — a WebSocket event stream is the
obvious first cut (matches the existing scan WebSocket); MQTT is a natural
alternative for a Home Assistant plugin to consume, but isn't committed
yet (see "Open threads").

**Concurrency**: enabling Home Automation Mode holds the keypad address
continuously, the same as the panel would see a real keypad permanently
installed. Any read-mode scan or future write-mode session run at the same
time needs the same keystroke-serialization the backend already owes
concurrent UI clients (see "Open threads" item on concurrency) — this
doesn't add a new problem, just another caller into that same
serialization point.

## Wireless (RF) zone visibility — datalogger role

Relevant to the **stationary datalogger** role above: testing against a real
Vista-20P + EVL4 (`envisalink_new`) established that the EVL4's TPI protocol
cannot see wireless-zone loop detail, and this is a firmware/protocol
limitation of the EVL4 specifically — not a property of the keypad bus it's
wired to.

- **What was tested:** a live in-service wireless zone in a genuine
  "CHECK 14" (RF supervision trouble) condition was monitored for ~14.5
  hours of debug-captured TPI traffic. The panel only ever sent repeating
  `%00` keypad-alpha frames (its own display text) — never the `%03`
  Realtime Contact-ID event, despite having CID codes defined for exactly
  this condition. Separately, an unprogrammed transmitter (serial
  `0231910`) was triggered ~30 times across open/close and tamper loops
  with continuous debug logging active; the EVL4 showed no trace of the
  serial or of any unrecognized command code — no indication an RF
  transmission occurred at all.
- **Why:** everything the EVL4 exposes over TPI (`%00` alpha updates, `%03`
  CID events) is the panel's own already-decided reporting, not a tap of
  the raw wireless-receiver data. The EVL4 is wired to the same 4-wire
  keypad bus a keypad uses, but its firmware only relays what the panel
  itself chooses to report, and never at per-loop (open/close vs. tamper
  vs. battery) or per-serial (unenrolled transmitter) granularity.
- **Contrast — AlarmDecoder (AD2Pi/AD2USB):** confirmed from
  `nutechsoftware/alarmdecoder`'s source, AD2 hardware taps the same kind of
  keypad bus, but at the receiver-broadcast level: the wireless receiver
  module (e.g. 5881ENL) puts every RF packet it hears directly onto the bus
  as a raw sentence, regardless of panel enrollment, because zone
  assignment is the panel's downstream decision, not the receiver's. AD2
  decodes this into `!RFX:<7-digit serial>,<hex>` lines, with the hex
  byte's bits carrying loop1-4/battery/supervision detail — for any
  transmitter in RF range, enrolled or not.
- **Implication for this hardware:** the RP2040 in this build clips onto the
  same physical keypad/ECP bus AD2 uses, not the EVL4's TPI abstraction —
  so it is electrically positioned to see the same raw receiver broadcasts
  AD2 sees. That is **not** automatic, though: the current firmware plan
  (porting esphome-vistaECP's `VistaECP` class) targets keystroke injection
  and alpha-display capture, not decoding the wireless-receiver sentence.
  Getting AD2-parity RF visibility (per-serial, per-loop, independent of
  panel zone programming) means adding that decode explicitly — it's a
  distinct, currently-unbuilt piece of firmware work, not a side effect of
  using a non-isolated bus tap. Tracked as an open thread below; relevant
  once the datalogger role is built out, not part of the near-term
  zone-discovery/read-write scope.

## Panel model roadmap

1. **Vista-20P** — done (validated, per the original protocol notes).
2. **Vista-10P / Vista-15P** — next. Program identically to the 20P;
   differences are limited to zone count and alpha-character limits, not
   protocol mechanics. Expected to be a low-effort addition once the 20P
   read/write is solid.
3. **Vista-128BPT** — after the small panels. A bigger commercial panel
   family. Note: the "T" series panels also expose a direct serial bus as
   an alternative connection mechanism alongside ECP — worth keeping in
   mind as a second transport option specifically for that panel family,
   separate from the Polling Loop work.

## Open threads

Carried forward from earlier discussion, still unresolved:

1. **Bench-validate RP2040 firmware** against a real Vista-20P via serial
   terminal before wiring in the rest of the build. First scope captures
   done pre-RP2040 (levels + timing) — see HARDWARE_ARCHITECTURE.md
   "Still open" item 1 for findings; full validation still waits on the
   RP2040 itself being wired in.
2. **Battery runtime budget** — deferred entirely; not needed during the
   development/testing phase, which runs on isolated wall power. Revisit
   once hardware bring-up is done and case dimensions are set.
3. **Write-mode keystroke sequences** — deferred as an add-on feature
   until the new hardware (RP2040) is confirmed working and ECP read-mode
   is solid on it. Will need to be worked out/documented with the user's
   help (protocol knowledge the assistant doesn't have yet) when that time
   comes.
4. **Concurrency at the firmware/backend level** — serializing real
   keystroke sends when multiple UI clients are connected, now that
   concurrent viewing is confirmed to be fine at the product level.
5. **PDF report export** — CSV export from a saved scan exists; PDF is the
   deferred half of the "Reports" tool entry.
6. **Reload a saved scan into a write-mode editor** — depends on write-mode
   existing at all (see item 3); the CSV `Save` output is meant to be that
   editor's eventual input format.
7. **Wireless receiver (RF) decode firmware** — see "Wireless (RF) zone
   visibility" above. Confirmed by testing that the EVL4/TPI path can't
   surface this data, and that AD2-style raw receiver-broadcast decoding
   would need to be added explicitly to the RP2040 firmware (not inherited
   for free from the non-isolated bus tap). Relevant to the stationary
   datalogger role; not part of near-term zone-discovery/read-write scope.
   The RP2040-Zero's Yellow bus-monitor tap (GP28, see
   HARDWARE_ARCHITECTURE.md "Bus coprocessor: RP2040-Zero") is the wiring
   this would build on, per esphome-vistaECP's own `MONITORTX` feature.
8. **AP/STA switching mechanism** — implementation choice (hostapd +
   wpa_supplicant + a watchdog script vs. NetworkManager vs. RaspAP) for
   the boot-into-AP / attempt-STA / 5-minute-timeout-fallback behavior in
   "Networking" above. Deferred, not blocking.
9. **Remote firmware-flashing mechanism** — the RP2040-Zero stayed on
   USB-serial specifically so the Pi can push new firmware without
   physical access (see HARDWARE_ARCHITECTURE.md "RP2040-Zero <-> Pi
   interconnect"), but the exact trigger (custom serial command vs.
   Arduino-Pico's built-in auto-reset convention) and whether to add a
   hardware BOOTSEL/RESET fallback for a bricked board are still open.
10. **Home Automation Mode wire format** — WebSocket event stream is the
   likely first cut (matches the existing scan WebSocket); MQTT is a
   natural alternative for a Home Assistant plugin to consume, but isn't
   committed yet. Needs deciding once this is actually being built.
11. **Home Automation Mode event granularity** — exactly which derived
    events to publish (per-zone open/close, per-partition
    armed/disarmed/alarm, raw alpha-display text, or all three) still
    needs deciding against real captured panel behavior, same care as the
    *56/*82 parsing corrections.
12. **Home Assistant plugin itself** — a separate codebase/deliverable
    (custom component consuming whatever wire format item 10 settles on),
    not part of this repo; not started.

## Resolved since first written

- **Display**: reversed from the previously decided 10.1in HDMI+USB-touch
  kiosk display (settled after initially considering 12in, rejected as too
  tablet-scale for a Pi build, and a pocketable 3-5in handheld, rejected
  once the "robust utility tool, not a cheap/clunky gadget" framing was
  clarified) to **no physical display at all** — the device is now fully
  headless, interacted with exclusively via browser. See "Hardware
  direction" and "Networking" above.
- **Networking**: wired Ethernet dropped along with the display; the
  device is WiFi-only now, boot-into-AP-mode-by-default with STA fallback
  after a 5-minute timeout, and WiFi credentials cleared on every reboot.
  See "Networking" above.
- **Isolation strategy**: flipped from "isolate the bus" to "non-isolated
  bus (best fidelity) + isolated power path (where the real ground-loop
  risk actually lives)". See HARDWARE_ARCHITECTURE.md.
- **Compute board**: Pi Zero 2 W remains the target but is unobtainable
  during the 2026 shortage; Raspberry Pi 4 decided as the interim/dev
  board, built to stay Zero-2W-compatible. See HARDWARE_ARCHITECTURE.md
  "Compute board: Pi 4 now, Zero 2 W target".
- **Bus coprocessor**: Waveshare RP2040-Zero decided, replacing the Pico.
  See HARDWARE_ARCHITECTURE.md "Bus coprocessor: RP2040-Zero".
- **RP2040 <-> Pi interconnect**: hardware UART over the GPIO header was
  decided, then reverted back to USB-serial once remote firmware flashing
  from the Pi came into scope — flashing needs USB (or SWD) regardless, so
  UART stopped saving anything and just added a second connection. See
  HARDWARE_ARCHITECTURE.md "RP2040-Zero <-> Pi interconnect".
- **RP2040-Zero / ECP interface pin mapping**: finalized against the
  board's actual pinout diagram (Green=GP26, Yellow=GP27, Yellow
  bus-monitor tap=GP28, WS2812 LED fixed on GP16), resolving the old
  GPIO_26 dual-assignment conflict. See HARDWARE_ARCHITECTURE.md "Bus
  coprocessor: RP2040-Zero".
- **Yellow/Green wire roles**: corrected on the bench — Yellow is the
  keypad→panel (TX) line here, Green is panel→keypad (RX), the reverse of
  what earlier drafts assumed from esphome-vistaECP's own README. Direct
  testing on this project's own wiring takes priority over the source
  project's color labels. See HARDWARE_ARCHITECTURE.md "Still open" item 1.
- **ESP32-as-host** (replacing the Pi entirely): considered and set
  aside — electrically viable over SPI, but would mean porting the entire
  backend to embedded C, a much bigger lift than deciding the RP2040↔Pi
  link alone. See HARDWARE_ARCHITECTURE.md "Considered and set aside".
- **Client-side (browser-held) data storage**: considered and rejected —
  it would break the stationary/unattended datalogger role (see "What this
  is" above), since there'd be no persistence without an actively open,
  connected browser tab. Storage stays on-device (industrial microSD, per
  HARDWARE_ARCHITECTURE.md).
