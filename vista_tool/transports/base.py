"""Transport abstraction between the zone-discovery walk logic and whatever
is actually wired to the panel.

Two implementations ship in this package:

- `tpi.EnvisalinkTPITransport` — talks TCP TPI to an Envisalink EVL3/EVL4
  module. Useful today (no custom hardware needed) and as a way to validate
  the walk/parse logic against real hardware before the handheld exists.
- `rp2040_serial.RP2040SerialTransport` — talks to an RP2040 coprocessor
  over USB serial. The RP2040 does the real-time ECP bus bit-banging
  (adapted from esphome-vistaECP's interrupt-driven library) and exposes a
  simple line protocol so the Pi side never has to deal with bus timing.

Both implementations produce the exact same event stream the zone_discovery
walk consumes, so the walk logic itself is transport-agnostic and was ported
unchanged from the notes.
"""

from __future__ import annotations

import abc
from dataclasses import dataclass


@dataclass(frozen=True)
class KeypadUpdate:
    """One alpha-display update from the panel's virtual keypad."""

    partition: int
    flags_hex: str
    alpha_text: str  # raw 32-char two-line display text, uninterpreted

    @property
    def is_disarmed(self) -> bool:
        # bit 0 of the Envisalink flags word is "armed" in this codebase's
        # convention (mirrors envisalink_new's handling) -- treat unknown/
        # unparseable flags as "not confirmed disarmed" (fail safe).
        try:
            flags = int(self.flags_hex, 16)
        except ValueError:
            return False
        ARMED_BIT = 0x01
        return not (flags & ARMED_BIT)


class PanelTransport(abc.ABC):
    """What the zone-discovery walk needs from a transport."""

    @abc.abstractmethod
    async def connect(self) -> None:
        ...

    @abc.abstractmethod
    async def close(self) -> None:
        ...

    @abc.abstractmethod
    async def send_keys(self, partition: int, keys: str) -> None:
        """Send a sequence of keypad characters, one at a time, with
        whatever inter-key pacing the transport requires. Must not return
        until all keys have been sent (acked), so callers can rely on
        ordering."""

    @abc.abstractmethod
    async def wait_for_display(
        self,
        partition: int,
        predicate,
        timeout: float,
        settle: float = 0.0,
    ) -> KeypadUpdate:
        """Wait until the most recent KeypadUpdate for `partition` satisfies
        `predicate(update) -> bool`.

        If settle > 0, the matching update must remain the latest update
        (i.e. no newer one supersedes it) for `settle` seconds before being
        returned -- this is "Pattern A" from the protocol notes. With
        settle == 0 this is "Pattern B": return as soon as the predicate
        matches, even if that's the display the panel was already showing.

        Raises asyncio.TimeoutError if `timeout` elapses first.
        """

    @abc.abstractmethod
    def last_update(self, partition: int) -> KeypadUpdate | None:
        """Most recently observed display state, if any, without waiting."""
