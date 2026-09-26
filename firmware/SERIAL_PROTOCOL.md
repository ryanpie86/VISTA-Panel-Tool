# Pi <-> RP2040 serial protocol

The RP2040 owns everything time-critical on the ECP bus (bit-level pulse
timing, per-device address slots, framing) -- adapted from the
interrupt-driven ECP library in `Dilbert66/esphome-vistaECP` (that project
only documents ESP8266/ESP32 pin assignments; this build's own Waveshare
RP2040-Zero pinout is finalized separately -- Yellow=GP26, Green=GP1,
Green bus-monitor tap=GP28 (Yellow=panel "data out", Green=keypad "data
in", per the Vista-20P's own technician manual, using the same
non-isolated resistor-divider + transistor bus-interface circuit as
esphome-vistaECP's ESP32 build, see `HARDWARE_ARCHITECTURE.md` "Bus
coprocessor: RP2040-Zero"). The Pi never
touches bus timing -- it only sees a simple newline-delimited text protocol
over USB-serial (a single USB-C cable, 115200 8N1; a UART-over-GPIO-header
plan was tried and reverted once remote firmware flashing from the Pi came
into scope -- see `HARDWARE_ARCHITECTURE.md` "RP2040-Zero <-> Pi
interconnect"), intentionally shaped like the Envisalink TPI lines so the
same `PushUpdatePollingTransport` base class and the same zone_discovery
walk logic work unmodified against either transport.

This file is the contract `rp2040_serial.py` implements against. The actual
RP2040 firmware lives in `rp2040_bridge/` (Arduino sketch, reusing
esphome-vistaECP's `Vista` class outside of ESPHome per its README, patched
for RP2040/arduino-pico) -- see `rp2040_bridge/README.md` for build
instructions, what got patched and why, and the current breadboard-stage
limitations (fixed keypad address, single partition, no
address-conflict detection yet).

## Pi -> RP2040

```
KEY,<partition>,<keys>\n
```
Send one or more virtual-keypad keystrokes, in order, as the emulated keypad
on the bus (`<keys>` is not limited to one character -- e.g. `KEY,1,4112800`
queues a whole 7-digit installer code in one command). RP2040 queues every
character immediately into the underlying ECP library's own outbound
buffer; the panel's real poll cycle then drains it at native bus speed.
This matters: waiting for an ACK after each individual character -- a full
USB round trip *and* a wait for the next real bus poll, every time -- was
bench-confirmed too slow for a multi-digit sequence, resetting the panel's
own inter-digit code-entry timeout before the sequence finished, even
though every individual key transmitted and acked fine on its own. Queuing
the whole batch up front instead mirrors how a human pressing keys on a
physical keypad only needs each press registered quickly, not a full
bus-poll round trip before the next press.

```
PING\n
```
Liveness check.

## RP2040 -> Pi

```
ACK,<partition>,<keys>\n
```
Confirms a KEY command's entire batch of keys was transmitted on the bus
(`<keys>` echoes exactly what the KEY command sent).

```
DISP,<partition>,<flags_hex>,<alpha_text>\n
```
Pushed whenever the emulated keypad's alpha display state changes (the
RP2040 is decoding the panel's own broadcast to *its own* virtual keypad
address, same as a real alpha keypad would show). `alpha_text` is the raw
32-character two-line display text -- no trimming or interpretation, exactly
like the TPI transport's alpha_text field. Framing rule mirrors TPI: since
alpha_text can theoretically contain a comma, the RP2040 must send it last
and the Pi parser must split on the first 3 commas only, not comma-split the
whole line. `flags_hex`'s bit layout is this firmware's own convention (not
copied from TPI/Envisalink) -- see `rp2040_bridge/README.md` "`flags_hex`
bit layout"; only bit 0 ("armed") is currently consumed downstream
(`vista_tool/transports/base.py`'s `KeypadUpdate.is_disarmed`).

```
ERR,<message>\n
```
Bus fault, framing error, or address-conflict detected (e.g. another real
keypad already owns the address this firmware is emulating -- must not
happen in the field; firmware should refuse to start if it detects its own
address already active).

```
PONG\n
```
Reply to PING.

## Notes carried over from the protocol notes doc

- There is no artificial pacing delay between KEY commands or between the
  keys within one KEY command's batch -- every key queues immediately into
  the underlying ECP library's own outbound buffer as soon as it arrives
  (rejected only if a previous batch is still pending), relying on that
  buffer and the panel's own real poll cycle to pace actual bus
  transmission. A firmware-side `delay()` used to throttle new KEY commands
  to once per ~0.5s; it was removed because it blocked the entire main loop
  (including RX/ACK-slot processing) for no protocol reason -- a real
  keypad has no equivalent restriction, and a tool user entering a time-
  sensitive code (e.g. a duress code) must not be made to wait on it. This
  differs from the TPI transport, which still sends and acks one key per
  command (Envisalink's own poll cycle to its keypad address is apparently
  fast enough for that to work there; this bus's isn't).
- The Pi-side wait-for-display patterns (settle-based "Pattern A" vs.
  poll-until-match "Pattern B") are unchanged -- see
  VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md section 3. This protocol only
  affects how a DISP event *arrives*, not how the walk logic waits for one.
- Safety rules (section 4) -- confirm disarmed via flags before starting,
  abort on S/N ambiguous, always send *99 on exit -- live entirely in
  zone_discovery.py and apply identically regardless of transport.
