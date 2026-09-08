"""Transport for the handheld's RP2040 bus coprocessor.

Speaks the line protocol defined in firmware/SERIAL_PROTOCOL.md over USB
serial. Requires `pyserial` (specifically `pyserial-asyncio` for the async
reader loop) -- add to requirements before running against real hardware.
"""

from __future__ import annotations

import asyncio
import logging

from .base import KeypadUpdate
from .polling_base import PushUpdatePollingTransport

logger = logging.getLogger(__name__)

KEY_ACK_TIMEOUT_SECONDS_PER_KEY = 3.0


class RP2040SerialTransport(PushUpdatePollingTransport):
    def __init__(self, device: str, baud: int = 115200) -> None:
        super().__init__()
        self.device = device
        self.baud = baud
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._reader_task: asyncio.Task | None = None
        self._pending_acks: dict[tuple[int, str], asyncio.Event] = {}

    async def connect(self) -> None:
        import serial_asyncio  # local import: optional dependency until hardware exists

        self._reader, self._writer = await serial_asyncio.open_serial_connection(
            url=self.device, baudrate=self.baud
        )
        self._reader_task = asyncio.create_task(self._read_loop())

    async def close(self) -> None:
        if self._reader_task:
            self._reader_task.cancel()
        if self._writer:
            self._writer.close()

    async def _read_loop(self) -> None:
        assert self._reader is not None
        while True:
            raw = await self._reader.readline()
            if not raw:
                break
            line = raw.decode(errors="replace").strip()
            if line:
                self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        if line.startswith("ACK,"):
            _, partition_s, keys = line.split(",", 2)
            key = (int(partition_s), keys)
            ev = self._pending_acks.get(key)
            if ev:
                ev.set()
            return
        if line.startswith("DISP,"):
            # DISP,<partition>,<flags_hex>,<alpha_text> -- alpha_text last,
            # split on first 3 commas only per SERIAL_PROTOCOL.md.
            parts = line.split(",", 3)
            if len(parts) != 4:
                logger.warning("Unparseable DISP line: %r", line)
                return
            _, partition_s, flags_hex, alpha_text = parts
            self._publish(
                KeypadUpdate(
                    partition=int(partition_s),
                    flags_hex=flags_hex,
                    alpha_text=alpha_text,
                )
            )
            return
        if line.startswith("ERR,"):
            logger.error("RP2040 reported bus error: %s", line[4:])
            return
        # PONG or unrecognized -- ignore

    async def send_keys(self, partition: int, keys: str) -> None:
        # Sent as one KEY command carrying the whole string, not one command
        # per character: the firmware queues every key immediately into the
        # underlying ECP library's own outbound ring buffer, which the
        # panel's real poll cycle then drains at native bus speed. Waiting
        # for an ACK after each individual character -- a full USB round
        # trip plus a wait for the next real bus poll, every time -- was
        # bench-confirmed too slow for a multi-digit code: the panel's own
        # inter-digit entry timeout reset before a 7-digit installer code
        # finished sending, even though every individual key transmitted
        # and acked fine on its own. See firmware/SERIAL_PROTOCOL.md.
        assert self._writer is not None
        if not keys:
            return
        key = (partition, keys)
        ack = asyncio.Event()
        self._pending_acks[key] = ack
        self._writer.write(f"KEY,{partition},{keys}\n".encode())
        await self._writer.drain()
        try:
            await asyncio.wait_for(ack.wait(), timeout=KEY_ACK_TIMEOUT_SECONDS_PER_KEY * len(keys))
        except asyncio.TimeoutError:
            logger.warning("No ACK from RP2040 for keys %r on partition %s", keys, partition)
        finally:
            self._pending_acks.pop(key, None)
