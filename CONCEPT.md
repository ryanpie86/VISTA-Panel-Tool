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
- Ground-isolated bus interface (the device has its own independent power
  source, so isolation protects against ground-potential differences
  between device and panel).
- LiPo battery + USB-C charging circuit that supports charging while
  running, for both portable use and overnight stationary logging.
- Industrial/endurance-rated microSD for storage.
- Display size still undecided (7-10.1in range) — see open threads.
- Enclosure: user-designed and 3D-printed; not a software/electrical
  concern for this doc.

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

**Write is next**, and is understood as the mirror-image operation:
navigating to the same menus/fields, but entering new values instead of
just reading and (for `*82`) mechanically re-saving what's already there.
The plan is a symmetric verify pattern: after any write, immediately
re-read the same field using the existing read mechanism to confirm the
panel actually accepted and stored the change, rather than trusting the
write blind. The exact keystroke sequences for entering edit mode and
committing new values (as opposed to just navigating and reading) still
need to be documented — this is real new protocol knowledge, not something
already captured in the existing notes, and will need the same
real-hardware care the original read-side work required (see the "six
real-hardware corrections" in the protocol notes as a cautionary example of
how easy this is to get subtly wrong).

Once read/write is solid for ECP, the roadmap continues into general I/O
(zone terminal voltage/resistance sensing — not a bus protocol) and Polling
Loop bus support (Vista 32/128/250 commercial panels — a current-loop
addressable-device protocol, electrically distinct from ECP). Both are
explicitly deferred until the ECP read/write utility is solid; the hardware
is meant to leave room for these as future add-on interface modules rather
than being redesigned for them later (see HARDWARE_ARCHITECTURE.md
"Modularity" section).

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

1. **Display size** — somewhere in the 7-10.1in range; depends on how much
   needs to be visible at once (simple form + zone list vs. side-by-side
   views).
2. **Bench-validate RP2040 firmware** against a real Vista-20P via serial
   terminal before wiring in the rest of the build.
3. **Battery runtime budget** — depends on final display choice.
4. **Write-mode keystroke sequences** — need to be worked out/documented
   with the user's help (protocol knowledge the assistant doesn't have yet)
   before any write code gets written.
5. **Concurrency at the firmware/backend level** — serializing real
   keystroke sends when multiple UI clients are connected, now that
   concurrent viewing is confirmed to be fine at the product level.
