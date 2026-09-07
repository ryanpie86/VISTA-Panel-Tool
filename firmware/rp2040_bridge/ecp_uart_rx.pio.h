#pragma once

#include "hardware/pio.h"

// ---------------------------------------------------------------------
// Pre-assembled from ecp_uart_rx.pio using the real Raspberry Pi pico-sdk
// pioasm tool (built from raspberrypi/pico-sdk, tools/pioasm -- verified
// output, not hand-encoded). Committed directly rather than relying on
// arduino-pico's automatic .pio -> .h build step, because that
// auto-detection did not fire on at least one real Arduino IDE toolchain
// during bench testing (reported as a plain "No such file or directory"
// on this exact filename). If your toolchain's auto-generation DOES work,
// it will conflict with this committed copy -- delete this file in that
// case, or make sure your build only sees one of the two.
//
// To regenerate by hand if ecp_uart_rx.pio ever changes: build pioasm
// from the pico-sdk repo (tools/pioasm; needs bison + flex, plain
// CMake/g++, no ARM cross-toolchain required) and run
// `pioasm -o c-sdk ecp_uart_rx.pio ecp_uart_rx.pio.h` -- then re-trim the
// output to just the fields below if you want to keep the same broad
// SDK-version compatibility this file was written for (see next
// paragraph).
//
// Deliberately uses only the long-stable pio_program struct fields
// (instructions/length/origin) rather than pioasm's newest output format,
// which also sets pio_version/used_gpio_ranges -- fields that may not
// exist on an older pico-sdk version than the one bundled with whatever
// arduino-pico release generated pioasm's output on the machine that
// produced this file. Omitting them is always safe (unset struct fields
// zero-initialize) on any SDK version that does have them.

#define ecp_uart_rx_wrap_target 0
#define ecp_uart_rx_wrap 4

static const uint16_t ecp_uart_rx_program_instructions[] = {
            //     .wrap_target
    0x2020, //  0: wait   0 pin, 0
    0xea27, //  1: set    x, 7                   [10]
    0x4001, //  2: in     pins, 1
    0x0642, //  3: jmp    x--, 2                 [6]
    0xb742, //  4: nop                           [23]
            //     .wrap
};

static const struct pio_program ecp_uart_rx_program = {
    .instructions = ecp_uart_rx_program_instructions,
    .length = 5,
    .origin = -1,
};

static inline pio_sm_config ecp_uart_rx_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + ecp_uart_rx_wrap_target, offset + ecp_uart_rx_wrap);
    return c;
}
