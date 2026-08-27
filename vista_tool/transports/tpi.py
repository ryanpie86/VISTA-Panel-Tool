"""Envisalink TPI transport (EVL3/EVL4, Honeywell/Vista mode).

Wire protocol per VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md section 2a:
- Send: `^03,<partition>,<char>$` per keystroke, ack'd with `^03,00$`.
- Receive: unsolicited `%00,<partition>,<flags_hex>,<field>,<beep_hex>,<alpha_text>$`
  keypad-update pushes. alpha_text may itself contain commas, so re-join
  everything past field 4 rather than splitting naively.

This transport is useful right now (works with hardware that already has an
Envisalink installed) and doubles as the easiest way to validate the ported
walk/parse logic in zone_discovery.py against real panel hardware before the
RP2040 handheld exists.
"""

from __future__ import annotations

import asyncio
import logging

from .base import KeypadUpdate
from .polling_base import PushUpdatePollingTransport

logger = logging.getLogger(__name__)

KEYSTROKE_PACING_SECONDS = 0.5  # notes section 2a: real panels need this


class EnvisalinkTPITransport(PushUpdatePollingTransport):
    def __init__(self, host: str, port: int, password: str) -> None:
        super().__init__()
        self.host = host
        self.port = port
        self.password = password
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._reader_task: asyncio.Task | None = None
        self._ack_events: dict[str, asyncio.Event] = {}

    async def connect(self) -> None:
        self._reader, self._writer = await asyncio.open_connection(self.host, self.port)
        self._reader_task = asyncio.create_task(self._read_loop())
        await self._login()

    async def _login(self) -> None:
        # Standard EVL login handshake: server sends a login prompt, client
        # replies with the password. Exact framing varies by firmware; this
        # is deliberately minimal -- swap in a maintained TPI login flow if
        # one is already vendored elsewhere in the project.
        assert self._writer is not None
        line = await self._reader.readline()  # e.g. b"login:\r\n" style prompt
        logger.debug("TPI login prompt: %r", line)
        self._writer.write(f"{self.password}\n".encode())
        await self._writer.drain()

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
            if not line:
                continue
            self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        if line.startswith("^03,"):
            # keypress ack
            self._ack_events.setdefault("ack", asyncio.Event()).set()
            return
        if line.startswith("%00,"):
            body = line[len("%00,"):].rstrip("$")
            # partition, flags_hex, field, beep_hex, then alpha_text (may
            # itself contain commas -- rejoin everything past field 4).
            parts = body.split(",", 4)
            if len(parts) != 5:
                logger.warning("Unparseable keypad update: %r", line)
                return
            partition_s, flags_hex, _field, _beep_hex, alpha_text = parts
            update = KeypadUpdate(
                partition=int(partition_s),
                flags_hex=flags_hex,
                alpha_text=alpha_text,
            )
            self._publish(update)

    async def send_keys(self, partition: int, keys: str) -> None:
        assert self._writer is not None
        for ch in keys:
            ack = asyncio.Event()
            self._ack_events["ack"] = ack
            self._writer.write(f"^03,{partition:02d},{ch}$\r\n".encode())
            await self._writer.drain()
            try:
                await asyncio.wait_for(ack.wait(), timeout=3.0)
            except asyncio.TimeoutError:
                logger.warning("No ack for keypress %r on partition %s", ch, partition)
            await asyncio.sleep(KEYSTROKE_PACING_SECONDS)
