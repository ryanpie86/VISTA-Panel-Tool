"""Shared wait_for_display implementation for transports that receive
KeypadUpdate events asynchronously (both TPI and the RP2040 serial link
work this way -- the display update is a push, not a request/response).

Subclasses just need to call `_publish(update)` whenever a new KeypadUpdate
arrives off the wire, and implement connect/close/send_keys themselves.
"""

from __future__ import annotations

import asyncio
import time

from .base import KeypadUpdate, PanelTransport


class PushUpdatePollingTransport(PanelTransport):
    def __init__(self) -> None:
        self._last: dict[int, KeypadUpdate] = {}
        self._last_change_ts: dict[int, float] = {}
        self._waiters: list[asyncio.Event] = []

    def _publish(self, update: KeypadUpdate) -> None:
        self._last[update.partition] = update
        self._last_change_ts[update.partition] = time.monotonic()
        for ev in self._waiters:
            ev.set()

    def last_update(self, partition: int) -> KeypadUpdate | None:
        return self._last.get(partition)

    async def wait_for_display(
        self,
        partition: int,
        predicate,
        timeout: float,
        settle: float = 0.0,
    ) -> KeypadUpdate:
        deadline = time.monotonic() + timeout
        poll_interval = 0.15

        while True:
            now = time.monotonic()
            if now > deadline:
                raise asyncio.TimeoutError(
                    f"partition {partition}: no matching display within {timeout}s "
                    f"(last seen: {self._last.get(partition)!r})"
                )

            current = self._last.get(partition)
            if current is not None and predicate(current):
                if settle <= 0:
                    return current
                # Pattern A: require the match to still be the latest value
                # after `settle` seconds with nothing newer superseding it.
                changed_at = self._last_change_ts.get(partition, 0.0)
                if time.monotonic() - changed_at >= settle:
                    return current

            await asyncio.sleep(poll_interval)
