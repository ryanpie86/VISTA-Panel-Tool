# RP2040 ECP bridge firmware

Implements `../SERIAL_PROTOCOL.md` on a Waveshare RP2040-Zero: emulates one
Vista alpha keypad on the ECP bus, and talks to the Pi over USB-serial. See
`HARDWARE_ARCHITECTURE.md` ("Bus coprocessor: RP2040-Zero") for the pin
assignments, divider values, and transistor sizing this firmware assumes.

## Building

1. Arduino IDE (2.x) with the
   [arduino-pico](https://github.com/earlephilhower/arduino-pico) board
   package installed (Boards Manager -> "Raspberry Pi Pico/RP2040" by Earle
   Philhower).
2. Board: **Waveshare RP2040-Zero** (listed under that board package once
   installed). USB Stack: default (Pico SDK).
3. Library: **Adafruit NeoPixel** (Library Manager) -- drives the onboard
   status LED on GP16.
4. Open `rp2040_bridge.ino` -- the Arduino IDE will pick up `vista.h`,
   `vista.cpp`, `ECPSoftwareSerial.h`, and `ECPSoftwareSerial.cpp` from the
   same folder automatically. Compile and upload over USB (BOOTSEL like
   any other RP2040 board for the first flash; later flashes can go over
   the same USB-serial port).

## What's vendored vs. written here

`vista.h`/`vista.cpp`/`ECPSoftwareSerial.h`/`ECPSoftwareSerial.cpp` are
pulled from `Dilbert66/esphome-components`
(`components/vista_alarm_panel/`) -- the same interrupt-driven ECP bus
library esphome-vistaECP uses, built standalone via its `ARDUINO_MQTT`
escape hatch (skips the ESPHome-specific includes). They're patched here
for RP2040/arduino-pico; every change is marked with a comment starting
"RP2040/Arduino-Pico port". In short:

- `ARDUINO_ARCH_RP2040` (auto-defined by the arduino-pico core) is turned
  into the `USE_RP2040` gate the upstream headers already had partial
  hooks for (`IRAM_ATTR` -> no-op, `ESP` -> `rp2040`).
- `disableInterrupts()`/`restoreInterrupts()` used Xtensa-only intrinsics
  (`xt_rsil`/`xt_wsr_ps`) that don't exist on the RP2040's Cortex-M0+;
  swapped for the Pico SDK's `save_and_disable_interrupts()`/
  `restore_interrupts()` (`hardware/sync.h`).
- `attachInterruptArg()` (ESP8266/ESP32 arg-passing variant) has no
  equivalent in arduino-pico's core. Since this firmware only ever runs a
  single `Vista` instance, `vista.cpp` routes through plain
  `attachInterrupt()` with a no-arg trampoline that reads the same
  file-scope instance pointer the constructor already sets
  (`pointerToVistaClass`).

Everything else in those four files -- the actual bit-timing (interrupt +
`micros()`-driven, portable), framing, and protocol decode -- needed no
changes; it was already written against the generic Arduino API.

`rp2040_bridge.ino` is new: it wires the vendored `Vista` class to the
`SERIAL_PROTOCOL.md` line protocol (`KEY`/`PING` in, `ACK`/`DISP`/`ERR`/
`PONG` out).

## Current limitations (breadboard bring-up stage)

- **Fixed keypad address.** `KEYPAD_ADDR` in the sketch is compile-time
  (default 16, the fixed address on Vista-20P/15P/10P panels -- see
  `CONCEPT.md` "Onboarding / keypad-address flow"). The scan-and-hot-swap
  address flow described there, and any address-selection command, isn't
  in `SERIAL_PROTOCOL.md` yet -- this firmware doesn't implement it.
- **Single partition.** Only one keypad address is emulated, so only
  partition 1 is accepted; `KEY` commands for any other partition get
  `ERR`.
- **No address-conflict detection.** `SERIAL_PROTOCOL.md` calls for
  refusing to start if this firmware's address is already active on the
  bus -- that needs the same bus-sniffing the onboarding flow will do, and
  isn't implemented yet. Only a generic "keybus not detected" fault is
  reported, tracked by the sketch itself off any successfully decoded ECP
  frame (`vista.keybusConnected` from the vendored library turned out to
  never actually be set `true` anywhere upstream -- dead state, not a real
  signal -- so this firmware doesn't rely on it).
- **No panel connected yet means no real transmit.** The ECP bus is
  poll/response: a keypad only gets to transmit when the panel polls its
  address. `ACK` only fires once the library's internal transmit queue
  actually drains (i.e. the panel polled and the pulse train went out);
  with nothing driving the bus, every `KEY` command will time out with
  `ERR` after ~4s. That's expected on a bare breadboard -- it's the
  intended behavior once wired to a real panel, not a bug to chase down
  before then.
- **What you *can* verify without a panel**: USB-CDC serial enumeration,
  `PING`/`PONG`, the onboard status LED (dim blue at boot, red once
  `keybusConnected` is false and the boot grace period elapses, green once
  it's true), and that GP26/GP28 read the expected idle levels through
  their dividers on a meter/scope.

## `flags_hex` bit layout

`SERIAL_PROTOCOL.md` deliberately mirrors the *shape* of Envisalink TPI's
`%00,...` line, but not its exact flag bits -- TPI's bit layout is
Envisalink's own, undocumented here, and not worth replicating byte-for-
byte for a single keypad address. This firmware defines its own single-
byte (2 hex digit) `flags_hex`, decoded from the same `statusFlagType`
struct `vista.h` already exposes:

| Bit | Meaning                          | Source field         |
|-----|-----------------------------------|-----------------------|
| 0   | Armed (any mode)                  | `armed`               |
| 1   | Armed away                        | `armedAway`           |
| 2   | Armed stay                        | `armedStay`           |
| 3   | Ready                              | `ready`               |
| 4   | Chime mode                        | `chime`               |
| 5   | Zone bypass active                | `zoneBypass`          |
| 6   | Alarm                              | `alarm`               |
| 7   | AC power present                  | `acPower`             |

Bit 0 is load-bearing today: `vista_tool/transports/base.py`'s
`KeypadUpdate.is_disarmed` reads it directly (`ARMED_BIT = 0x01`). The rest
are populated but not yet consumed by anything in `vista_tool/` -- add
readers as needed rather than renumbering these bits.
