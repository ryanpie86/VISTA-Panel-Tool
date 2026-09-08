"""Send a sequence of ECP keypresses to the RP2040 bridge, bench-testing tool.

Manually re-typing/pasting one `KEY,<partition>,<char>` serial command at a
time is slow enough (human copy-paste + window-switching, easily several
seconds per key) that it can outlast the panel's own inter-digit code-entry
timeout -- each key still ACKs individually (that only confirms the RP2040
transmitted it during a real poll of its keypad address), but the panel
resets its "how many digits of the code have I seen so far" progress before
a multi-digit sequence like an installer code completes, so nothing appears
to happen even though every key is being delivered.

This drives the existing RP2040SerialTransport.send_keys() helper instead,
which already sends the next key as soon as the previous one's ACK (or a
3s timeout) comes back -- limited only by the firmware's own 500ms
inter-key pacing, not by how fast a human can paste.

Usage:
    python scripts/send_keys.py /dev/ttyACM0 4112800
    python scripts/send_keys.py /dev/ttyACM0 '*99' --partition 1

Prints the partition's display (DISP) text after each key so you can watch
the panel's prompt advance in real time.
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
        for ch in args.keys:
            await transport.send_keys(args.partition, ch)
            update = transport.last_update(args.partition)
            shown = update.alpha_text if update else "(no display update seen yet)"
            print(f"sent {ch!r} -> {shown!r}")
    finally:
        await transport.close()


if __name__ == "__main__":
    asyncio.run(main())
