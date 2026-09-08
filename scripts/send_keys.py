"""Send a sequence of ECP keypresses to the RP2040 bridge, bench-testing tool.

Sending keys one at a time -- whether by hand (pasting a `KEY,<partition>,
<char>` serial command per key) or by waiting for each key's own ACK before
sending the next -- is bench-confirmed too slow for a multi-digit sequence
like an installer code: each ACK only confirms the RP2040 transmitted that
key during a real poll of its keypad address, which still costs a full USB
round trip plus a wait for the next bus poll cycle, every single time. Chain
enough of those and the panel's own inter-digit code-entry timeout resets
before the sequence finishes, even though every individual key transmitted
and acked just fine on its own.

This sends the whole key sequence as ONE `KEY,<partition>,<keys>` command
(see firmware/SERIAL_PROTOCOL.md) via RP2040SerialTransport.send_keys().
The firmware queues every key immediately into the underlying ECP library's
own outbound buffer, and the panel's real poll cycle drains it at native bus
speed from there -- same as how a human pressing keys on a physical keypad
only needs each press registered quickly, not a full bus-poll round trip
before the next press.

Usage:
    python scripts/send_keys.py /dev/ttyACM0 4112800
    python scripts/send_keys.py /dev/ttyACM0 '*99' --partition 1

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
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    transport = RP2040SerialTransport(device=args.device, baud=args.baud)
    await transport.connect()
    try:
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
