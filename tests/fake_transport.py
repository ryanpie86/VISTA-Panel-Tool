"""A scripted fake transport for exercising zone_discovery.py without any
hardware, in the same spirit as envisalink_new's
test_honeywell_zone_discovery.py fake-client test suite (notes section 10).
"""

from __future__ import annotations

import asyncio

from vista_tool.transports.base import KeypadUpdate
from vista_tool.transports.polling_base import PushUpdatePollingTransport


class FakeTransport(PushUpdatePollingTransport):
    """Feeds pre-scripted display responses to whatever the walk sends.

    `script` maps a predicate function `(partition, keys_sent) -> str | None`
    to an alpha_text to publish immediately after those keys are sent. This
    keeps tests declarative and close to the real captured strings in the
    protocol notes (section 6/8) rather than reimplementing panel behavior.
    """

    def __init__(self, responder) -> None:
        super().__init__()
        self.responder = responder
        self.sent_log: list[tuple[int, str]] = []
        # Start "disarmed" so _require_disarmed() passes by default.
        self._publish(KeypadUpdate(partition=1, flags_hex="00", alpha_text=" " * 32))

    async def connect(self) -> None:
        pass

    async def close(self) -> None:
        pass

    async def send_keys(self, partition: int, keys: str) -> None:
        self.sent_log.append((partition, keys))
        alpha_text = self.responder(partition, keys)
        if alpha_text is not None:
            self._publish(KeypadUpdate(partition=partition, flags_hex="00", alpha_text=alpha_text))
        await asyncio.sleep(0)  # yield control, keep tests fast
