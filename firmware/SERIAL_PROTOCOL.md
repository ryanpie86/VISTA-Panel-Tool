# Pi <-> RP2040 serial protocol

The RP2040 owns everything time-critical on the ECP bus (bit-level pulse
timing, per-device address slots, framing) -- adapted from the
interrupt-driven ECP library in `Dilbert66/esphome-vistaECP` (that project
already documents a stock RP2040 Pico pinout: RX/yellow=GPIO21, TX/green=GPIO20,
monitor=GPIO18, using the same non-isolated resistor-divider + transistor
bus-interface circuit as its ESP32 build). The Pi never touches bus timing --
it only sees a simple newline-delimited text protocol over USB serial
(115200 8N1), intentionally shaped like the Envisalink TPI lines so the same
`PushUpdatePollingTransport` base class and the same zone_discovery walk
logic work unmodified against either transport.

This file is the contract `rp2040_serial.py` implements against. The actual
RP2040 firmware (PlatformIO/Arduino, reusing esphome-vistaECP's ECP class
outside of ESPHome per its README) is a separate build -- not included here.

## Pi -> RP2040

```
KEY,<partition>,<char>\n
```
Send one virtual-keypad keystroke as the emulated keypad on the bus. RP2040
handles the actual pulse-train transmission and required inter-key pacing.

```
PING\n
```
Liveness check.

## RP2040 -> Pi

```
ACK,<partition>,<char>\n
```
Confirms a KEY command was transmitted on the bus.

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
whole line.

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

- Keystroke pacing (~0.5s) is enforced firmware-side, not by the Pi -- the
  Pi's `send_keys()` just waits for each ACK before sending the next key,
  same call shape as the TPI transport.
- The Pi-side wait-for-display patterns (settle-based "Pattern A" vs.
  poll-until-match "Pattern B") are unchanged -- see
  VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md section 3. This protocol only
  affects how a DISP event *arrives*, not how the walk logic waits for one.
- Safety rules (section 4) -- confirm disarmed via flags before starting,
  abort on S/N ambiguous, always send *99 on exit -- live entirely in
  zone_discovery.py and apply identically regardless of transport.
