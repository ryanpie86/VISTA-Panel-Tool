"""Passively log everything the RP2040 bridge reports, without sending
any keys ourselves.

The bridge's Green monitor tap (PIN_GREEN_MON) is a passive hardware tap
on the shared ECP bus, independent of our own TX -- it sees any device's
traffic, including a real keypad's. Run this while pressing keys on the
physical keypad to capture GREENRAW/GREENEDGE bytes and the surrounding
Yellow-side RAW frames (e.g. the F6 invite/poll for that keypad's own
address) for a real, known-good key-send -- ground truth to diff against
what our own writeChars() generates.

Usage:
    python scripts/log_serial.py /dev/ttyACM0

Ctrl+C to stop.
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
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    # Timestamps here (unlike send_keys.py/spam_key.py) so a later review
    # can line up which log lines happened while a given key was pressed.
    logging.basicConfig(level=logging.INFO, format="%(asctime)s.%(msecs)03d %(message)s", datefmt="%H:%M:%S")

    transport = RP2040SerialTransport(device=args.device, baud=args.baud)
    await transport.connect()
    print("Listening -- press keys on the physical keypad now. Ctrl+C to stop.")
    try:
        await asyncio.Event().wait()  # block forever; _read_loop() does the logging
    finally:
        await transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
