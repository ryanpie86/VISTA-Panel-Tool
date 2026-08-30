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
2. **Stationary datalogger** — can be left connected and powered at a panel
   for extended unattended periods (overnight or longer) for
   troubleshooting, reachable remotely the whole time.

Both roles run on the same hardware and the same software — there's no
separate "logger mode" device, just the same tool used differently.

## Hardware direction (summary — full detail in HARDWARE_ARCHITECTURE.md)

- Raspberry Pi 4 (8GB) + RP2040 (Pico) coprocessor for real-time ECP bus
  timing. Android-tablet and fully-custom-tablet alternatives were both
  considered and rejected: Android would require sideloading/rooting to
  get USB-serial access to custom hardware, which the user doesn't want;
  a fully custom 12in build was more hardware R&D than warranted once a
  Pi-based approach was back on the table.
- Non-isolated ECP bus interface (shares ground with the panel, same as a
  real physical keypad, for full signal fidelity) — isolation instead lives
  on the power path (see below), which is where the actual ground-loop
  risk from USB-C charging sits. Full reasoning in
  HARDWARE_ARCHITECTURE.md's "Isolation strategy" section.
- LiPo battery + USB-C charging circuit on an isolated DC-DC/charge path,
  supporting charging while running, for both portable use and overnight
  stationary logging.
- Industrial/endurance-rated microSD for storage.
- GeeekPi 10.1in HDMI+USB-touch display, 1280x800, kiosk/kickstand form
  factor -- a small device that sits upright at an angle at the panel, not
  a laptop-style device held/carried while in use.
- Enclosure: user-designed and 3D-printed, with a kickstand; not a
  software/electrical concern for this doc.

## Networking

All three of these are in scope from the start, not sequenced as
nice-to-haves:

- **WiFi client** — joins existing site WiFi when available, same as a
  laptop would.
- **WiFi access-point / hotspot mode** — the device broadcasts its own
  network. This is the auto-fallback: if no known WiFi is available (or as
  the default, depending on final UX), a tech's laptop or phone joins the
  device's hotspot directly and gets the same web UI a local touchscreen
  would show. No dependency on site infrastructure.
- **Wired Ethernet** — available via the Pi 4's built-in port, useful for
  the stationary/overnight logging case where a wired drop is more
  reliable than WiFi.

The web UI is the same regardless of client — on-device touchscreen and a
remote browser are just two clients of the same local FastAPI server.
**Concurrent sessions are explicitly fine** — multiple viewers (or the
touchscreen plus a remote browser) can be connected at once. This does NOT
mean multiple uncoordinated keystroke streams reach the panel — the
backend/firmware layer is responsible for serializing actual writes to the
bus regardless of how many UI clients are watching or interacting; that's
an implementation detail below the product-level "concurrent viewing is
fine" decision.

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
operating, and get ECP *reading* working end-to-end on that hardware,
before write-mode is touched at all. Write is still understood as the
eventual mirror-image operation — navigating to the same menus/fields but
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
   terminal before wiring in the rest of the build.
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
5. **RP2040 / ECP interface pin mapping** — esphome-vistaECP's reference
   schematics only document ESP8266/ESP32 pin assignments, not RP2040.
   Deliberately not being worked out yet — hardware isn't in hand. Will be
   done pin-by-pin once the RP2040 and interface components are physically
   available.
6. **PDF report export** — CSV export from a saved scan exists; PDF is the
   deferred half of the "Reports" tool entry.
7. **Reload a saved scan into a write-mode editor** — depends on write-mode
   existing at all (see item 3); the CSV `Save` output is meant to be that
   editor's eventual input format.
8. **Wireless receiver (RF) decode firmware** — see "Wireless (RF) zone
   visibility" above. Confirmed by testing that the EVL4/TPI path can't
   surface this data, and that AD2-style raw receiver-broadcast decoding
   would need to be added explicitly to the RP2040 firmware (not inherited
   for free from the non-isolated bus tap). Relevant to the stationary
   datalogger role; not part of near-term zone-discovery/read-write scope.

## Resolved since first written

- **Display**: 10.1in HDMI+USB-touch, kiosk-style with a kickstand -- not
  laptop-style. Settled after initially considering 12in (rejected as too
  tablet-scale for a Pi build) and a pocketable 3-5in handheld (rejected
  once the "robust utility tool, not a cheap/clunky gadget" framing was
  clarified).
- **Isolation strategy**: flipped from "isolate the bus" to "non-isolated
  bus (best fidelity) + isolated power path (where the real ground-loop
  risk actually lives)". See HARDWARE_ARCHITECTURE.md.
