"""Send a sequence of ECP keypresses to the RP2040 bridge, bench-testing tool.

By default this sends the whole key sequence as ONE `KEY,<partition>,<keys>`
command (see firmware/SERIAL_PROTOCOL.md) via RP2040SerialTransport.send_keys().
The firmware queues every key immediately, and Vista::writeChars() batches
everything still queued at send time into a single multi-byte ECP frame
(header, length, all data bytes, one checksum) rather than one frame per key.

That batching was written to work around single-key sends being too slow
for a multi-digit code -- but every real keypad frame captured off the bus
so far (via scripts/log_serial.py, entering a code by hand) has length=2:
real keypads never send more than one key per frame, ever, even for a fast
multi-digit entry. Nothing has actually confirmed the panel accepts a
batched multi-key frame; the original "too slow" finding predates fixes to
two real bugs (an interrupt/re-entrancy issue and, much more seriously, a
TX idle-level bug that held the bus jammed) that could easily have caused
that conclusion on their own. --one-at-a-time tests the batching hypothesis
directly: it sends each character as its own `KEY,<partition>,<char>`
command in sequence, producing one length=2 frame per key, matching what a
real keypad actually puts on the bus.

Usage:
    python scripts/send_keys.py /dev/ttyACM0 4112800
    python scripts/send_keys.py /dev/ttyACM0 '*99' --partition 1
    python scripts/send_keys.py /dev/ttyACM0 4112800 --one-at-a-time

Prints the partition's display (DISP) text once the whole batch is
confirmed sent (or once the ACK wait times out).
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from vista_tool.transports.rp2040_serial import RP2040SerialTransport  # noqa: E402


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="Serial device, e.g. /dev/ttyACM0")
    parser.add_argument("keys", help="Keys to send in order, e.g. 4112800 or '*99'")
    parser.add_argument("--partition", type=int, default=1)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--one-at-a-time",
        action="store_true",
        help="Send each character as its own KEY command (one length=2 ECP frame per key, "
        "matching real keypad traffic) instead of batching the whole string into one frame.",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    transport = RP2040SerialTransport(device=args.device, baud=args.baud)
    await transport.connect()
    try:
        if args.one_at_a_time:
            for key in args.keys:
                print(f"--- sending {key!r} ---")
                await transport.send_keys(args.partition, key)
        else:
            await transport.send_keys(args.partition, args.keys)
        # send_keys() returns as soon as the ACK line is processed, which
        # can race the DEBUG line the firmware sends right after it (and
        # any DISP update the panel broadcasts shortly after) -- give the
        # background read loop a moment to catch up before printing/
        # closing, so both actually show up here instead of getting
        # dropped when the connection tears down.
        await asyncio.sleep(1.0)
        update = transport.last_update(args.partition)
        shown = update.alpha_text if update else "(no display update seen yet)"
        print(f"sent {args.keys!r} -> {shown!r}")
    finally:
        await transport.close()


if __name__ == "__main__":
    asyncio.run(main())
