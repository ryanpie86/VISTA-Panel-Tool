"""Send the same key over and over, hands-free, so you can hold a scope
probe steady instead of retyping a command between captures.

Each send already takes a couple of seconds to resolve on its own (the
firmware announces our keypad address, waits for the panel's invite, times
out, and retries a few times before giving up -- see DEBUG line output),
so this can't literally guarantee a fixed 1-second cadence; it just fires
the next send immediately after the previous one resolves, then sleeps
whatever's left of --interval (usually nothing). What you get in practice
is a continuous, repeating stream of send attempts to trigger a scope on,
back to back, for as long as this keeps running.

Usage:
    python scripts/spam_key.py /dev/ttyACM0 9
    python scripts/spam_key.py /dev/ttyACM0 9 --interval 2 --partition 1

Ctrl+C to stop.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from vista_tool.transports.rp2040_serial import RP2040SerialTransport  # noqa: E402


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="Serial device, e.g. /dev/ttyACM0")
    parser.add_argument("key", help="Key to send repeatedly, e.g. 9")
    parser.add_argument("--interval", type=float, default=1.0, help="Seconds between send attempts (default 1.0)")
    parser.add_argument("--partition", type=int, default=1)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    transport = RP2040SerialTransport(device=args.device, baud=args.baud)
    await transport.connect()
    count = 0
    try:
        while True:
            count += 1
            started = time.monotonic()
            print(f"--- attempt {count} ---")
            await transport.send_keys(args.partition, args.key)
            elapsed = time.monotonic() - started
            remaining = args.interval - elapsed
            if remaining > 0:
                await asyncio.sleep(remaining)
    finally:
        print(f"\nStopped after {count} attempts.")
        await transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
