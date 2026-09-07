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
   same folder automatically, along with `ecp_uart_rx.pio.h` (see "PIO
   code generation" below -- committed pre-built, not auto-generated from
   `ecp_uart_rx.pio` at build time; that was tried first and didn't work
   on at least one real toolchain).
5. Compile and upload over USB (BOOTSEL like any other RP2040 board for
   the first flash; later flashes can go over the same USB-serial port).

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
- `IRAM_ATTR` on RP2040 was upstream's own no-op hook -- meaning the
  interrupt-driven bit-sampling functions ran from flash (XIP) instead of
  RAM, unlike ESP8266 where `IRAM_ATTR` is load-bearing for exactly that
  timing guarantee. Redefined to `__attribute__((section(".time_critical")))`,
  which the Pico SDK's linker script pulls into RAM (same mechanism the
  SDK's own `__not_in_flash_func()` uses). `digitalRead()` in the two
  ISR-path reads was also swapped for the Pico SDK's direct `gpio_get()`.
  Neither change was enough on its own -- see the PIO section below.
- **Primary RX byte assembly moved off the software bit sampler entirely,
  onto a PIO state machine** (`ecp_uart_rx.pio`, wired up by
  `Vista::pioRxInit()`/`pioRxPump()` in `vista.cpp`, new methods not
  present upstream). Bench testing showed the original interrupt-driven
  software decoder reliably loses sync partway through the one long
  (44-byte, ~100ms) F7 status frame specifically once real bus traffic
  (e.g. a physical keypad in active use) overlaps it -- consistent with
  an occasional missed GPIO edge under CPU/interrupt load, something
  `IRAM_ATTR`/`gpio_get()` alone measurably didn't fix. PIO samples GPIO
  with dedicated hardware timing, independent of whatever the CPU is
  doing, which removes that failure mode rather than reducing its odds.
  See "PIO-based RX" below for how this is scoped and wired in.
  `pushByte()` (new, in `ECPSoftwareSerial.h`/`.cpp`) is the hand-off
  point: it does the exact same overflow-checked insertion `rxBits()`
  already did into `SoftwareSerial`'s byte buffer, so `available()`/
  `read()`/`overflow()` and everything downstream of them (`readChars()`,
  `decodePacket()`, the whole rest of `vista.cpp`) are completely
  unchanged and can't tell whether a byte came from PIO or the software
  path.

Everything else in those four files -- the actual bit-timing (interrupt +
`micros()`-driven, portable), framing, and protocol decode -- needed no
changes; it was already written against the generic Arduino API.

`rp2040_bridge.ino` is new: it wires the vendored `Vista` class to the
`SERIAL_PROTOCOL.md` line protocol (`KEY`/`PING` in, `ACK`/`DISP`/`ERR`/
`PONG` out).

## PIO-based RX

`ecp_uart_rx.pio` is a new file (not from upstream) implementing this
bus's actual framing (4800 baud, 8 data bits, even parity, 2 stop bits --
confirmed by oscilloscope cursor timing: one 12-bit byte at 4800 baud is
~2.5ms, matching a measured 2.46ms inter-byte span to within 2%). Its
"wait for start bit / center on first data bit / shift in 8 bits" front
half is reused unmodified from the Pico SDK's own `uart_rx.pio` reference
example; only the tail differs, to skip the parity bit + 2nd stop bit
(this bus's framing, not 8-N-1) rather than checking a single stop bit.
Parity is deliberately not validated in PIO -- matching how the existing
software decoder's own `checkParity()` already treats a mismatch as
non-fatal (logs a warning, keeps the byte anyway), so skipping it entirely
in PIO loses no real protection.

Scope is deliberately narrow: **only** the primary RX pin (Yellow/GP26)
uses PIO. The Green monitor pin (GP28) and TX both still use the original
software bit-bang path; neither has shown this failure, so neither was
touched.

PIO's enable/disable state is gated directly off `Vista::rxHandleISR()`'s
existing `_rxState` bus-protocol state machine (preamble detection,
ACK-slot timing) -- PIO itself can't tell a genuine ~208us start bit from
the bus's multi-millisecond preamble/ACK pulses, so it's only allowed to
run during `_rxState==sNormal` (the real data window), reset cleanly
(FIFOs cleared, restarted at the program's first instruction) every time
that window opens.

Live-traffic bench testing (real Vista-20P, concurrent keypad activity)
found one more failure mode in that gating: the `_highTime > 6000us`
check `rxHandleISR()` uses to recover from `sNormal` if a frame stalls is
itself edge-interrupt-driven, and under heavy bus load (frequent F0
polls) its own edge servicing can lag enough to look like a 6ms+ gap
occurred mid-frame even though PIO, sampling in hardware, was still
receiving real bytes the whole time -- disabling PIO partway through an
F7 frame and truncating it (observed consistently around byte 12-13 of
45). `Vista::_f7LongReadActive` (set only around the F7 payload's long
`readChars()` call) tells that check to stand down for the duration,
since the long read's own 20ms poll-loop timeout is what should decide
whether the frame actually stalled, not edge timing that's known to be
unreliable under exactly this load. The ordinary short-frame recovery
that check exists for elsewhere is untouched.

If `RAW`/`RAWF7` dumps still come back truncated after this fix, suspect
the same class of problem elsewhere in the `_rxState` machine before
assuming the PIO program itself (`ecp_uart_rx.pio`, `Vista::pioRxInit()`)
is wrong -- its cycle counts and clock-divider math were verified against
a real oscilloscope capture and haven't been the source of truncation in
testing so far.

## PIO code generation

`ecp_uart_rx.pio.h` is committed directly rather than relying on
arduino-pico's documented "auto-assemble any `.pio` file in the sketch
folder" build step -- that was the original plan, but it produced a plain
"No such file or directory" on `ecp_uart_rx.pio.h` on a real Arduino IDE
toolchain during bench testing, so it isn't being relied on. If your setup
*does* support it, delete the committed `ecp_uart_rx.pio.h` (or make sure
your build doesn't see both a generated and a committed copy at once).

The committed header's instruction encoding is **not hand-written** --
it's the verified output of the real Raspberry Pi pico-sdk `pioasm` tool,
built from source for this purpose (`tools/pioasm` in the `pico-sdk` repo;
it's a host-native C++/CMake project needing only `bison`+`flex`, no ARM
cross-toolchain) and run against `ecp_uart_rx.pio` directly, then trimmed
to the long-stable `pio_program` struct fields
(`instructions`/`length`/`origin`) for broad SDK-version compatibility --
see the comment at the top of the file for the exact regeneration command
if `ecp_uart_rx.pio` ever changes.

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
