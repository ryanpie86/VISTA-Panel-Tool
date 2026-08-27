"""Vista-20P *56 (zone type) and *82 (alpha descriptor) discovery walks.

Ported from ryanpie86/envisalink_new's honeywell_zone_discovery.py per
VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md, generalized to run over any
PanelTransport (Envisalink TPI today, RP2040 direct-bus tomorrow) instead of
being tied to a Home Assistant / pyenvisalink client.

Every safety rule in the notes' section 4 is preserved exactly:
  1. Refuse to start unless the partition is confirmed disarmed.
  2. *56 never advances past the per-zone SUMMARY SCREEN.
  3. *82 has no read-only view -- every "read" is mechanically a re-save of
     the unchanged descriptor. This is inherent to the panel, not a bug.
  4. Abort immediately if a captured display contains S/N, LOOP, or XMIT
     (wireless transmitter-enrollment prompt -- unsupported, unexplored).
  5. The whole run has an overall timeout sized to zone_count, not a flat
     guess (see notes section 10).
  6. *99 (program-mode exit) always runs, success or failure, via try/finally.
"""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass, field
from typing import AsyncIterator, Literal

from .transports.base import PanelTransport
from .zone_types import describe

logger = logging.getLogger(__name__)

MIN_ZONE = 1
MAX_ZONE = 64  # per notes section 11: 91-99 are zone *type* codes, never scan as zones

# Per-step timeouts (notes section 3/7)
STEP_TIMEOUT_SECONDS = 8.0
SETTLE_SECONDS = 0.4
POST_SAVE_PACING_SECONDS = 1.0  # *82 round 6 correction: don't wait-for-change here

# Overall timeout sizing (notes section 10) -- generous above measured pace
BASE_OVERHEAD_SECONDS = 15.0
PER_ZONE_TYPE_BUDGET_SECONDS = 3.0
PER_ZONE_NAME_BUDGET_SECONDS = 3.5

WIRELESS_ENROLLMENT_MARKERS = ("S/N", "LOOP", "XMIT")


class WalkAborted(RuntimeError):
    """Raised when a captured display looks unsafe to continue past."""


@dataclass
class ZoneResult:
    zone: int
    zone_type_code: str | None = None
    zone_type_label: str | None = None
    name: str | None = None
    raw_summary: str | None = None
    raw_descriptor: str | None = None


@dataclass
class ScanProgress:
    stage: Literal["types", "names", "done", "error"]
    zone: int | None = None
    total_zones: int = 0
    message: str = ""


def compute_overall_timeout(zone_count: int, read_names: bool) -> float:
    """Notes section 10: scale the hard overall ceiling with zone count."""
    total = BASE_OVERHEAD_SECONDS + zone_count * PER_ZONE_TYPE_BUDGET_SECONDS
    if read_names:
        total += zone_count * PER_ZONE_NAME_BUDGET_SECONDS
    return total


def parse_zone_type_summary(alpha_text: str) -> str:
    """Notes section 6: slice chars 16-32 (data row) BEFORE whitespace-
    splitting -- the header's last field runs into the data row's first
    field with no space, corrupting a naive whole-string split."""
    if len(alpha_text) < 32:
        raise ValueError(f"Zone summary text too short: {alpha_text!r}")
    data_row = alpha_text[16:32]
    tokens = data_row.split()
    if len(tokens) < 2:
        raise ValueError(f"Could not parse zone type from data row: {data_row!r}")
    return tokens[1]  # second token is the 2-digit zone type code


def parse_zone_descriptor(alpha_text: str) -> str:
    """Notes section 8: strip the fixed 9-char header, then collapse all
    whitespace runs in the remainder to single spaces. Do NOT just
    .strip() the raw capture -- that leaves the header and internal
    double-space artifacts in the "name"."""
    if len(alpha_text) < 9:
        raise ValueError(f"Descriptor text too short: {alpha_text!r}")
    payload = alpha_text[9:]
    return " ".join(payload.split())


def _check_for_wireless_enrollment(alpha_text: str) -> None:
    for marker in WIRELESS_ENROLLMENT_MARKERS:
        if marker in alpha_text:
            raise WalkAborted(
                f"Display contains {marker!r} -- looks like a wireless "
                f"transmitter-enrollment prompt, aborting: {alpha_text!r}"
            )


class ZoneDiscoveryWalk:
    """Drives *56 and/or *82 against a single partition."""

    def __init__(
        self,
        transport: PanelTransport,
        installer_code: str,
        partition: int = 1,
    ) -> None:
        self.transport = transport
        self.installer_code = installer_code
        self.partition = partition

    async def _require_disarmed(self) -> None:
        last = self.transport.last_update(self.partition)
        if last is None or not last.is_disarmed:
            raise WalkAborted(
                "Partition is not confirmed disarmed -- refusing to enter "
                "installer programming mode (safety rule 1)."
            )

    async def _send_and_wait(
        self, keys: str, predicate, settle: float = SETTLE_SECONDS
    ):
        await self.transport.send_keys(self.partition, keys)
        update = await self.transport.wait_for_display(
            self.partition, predicate, STEP_TIMEOUT_SECONDS, settle=settle
        )
        _check_for_wireless_enrollment(update.alpha_text)
        return update

    async def _exit_programming_mode(self) -> None:
        try:
            await self.transport.send_keys(self.partition, "*99")
        except Exception:
            logger.exception("Failed to send *99 exit -- panel may still be in programming mode")

    async def run(
        self,
        zones: list[int] | None = None,
        read_types: bool = True,
        read_names: bool = False,
    ) -> AsyncIterator[ScanProgress | ZoneResult]:
        """Async generator: yields ScanProgress updates and ZoneResult
        objects as they complete, so a web UI can show live per-zone
        progress instead of a spinner (notes section 10)."""
        zones = zones or list(range(MIN_ZONE, MAX_ZONE + 1))
        overall_timeout = compute_overall_timeout(len(zones), read_names)

        await self._require_disarmed()

        results: dict[int, ZoneResult] = {z: ZoneResult(zone=z) for z in zones}

        async def _walk():
            if read_types:
                async for item in self._walk_56(zones, results):
                    yield item
            if read_names:
                async for item in self._walk_82(zones, results):
                    yield item

        try:
            async with asyncio.timeout(overall_timeout):
                async for item in _walk():
                    yield item
        except (TimeoutError, asyncio.TimeoutError) as exc:
            yield ScanProgress(stage="error", total_zones=len(zones), message=str(exc))
        except WalkAborted as exc:
            yield ScanProgress(stage="error", total_zones=len(zones), message=str(exc))
        finally:
            await self._exit_programming_mode()

        yield ScanProgress(stage="done", total_zones=len(zones))

    # ---- *56 zone type walk (notes section 5-6) ----

    async def _walk_56(self, zones: list[int], results: dict[int, ZoneResult]):
        await self._send_and_wait(f"{self.installer_code}800", lambda u: True)
        await self._send_and_wait("*56", lambda u: True)
        await self._send_and_wait("0", lambda u: True)  # "SET TO CONFIRM?" -> no, once per session

        for zone in zones:
            zz = f"{zone:02d}"
            # Unlike *82, the *56 summary screen's header text is fixed
            # ("Zn ZT P RC HW:RT" / "Zn ZT P RC IN:L ") and never contains
            # the zone number itself (notes section 6) -- there is no
            # zone-specific string to poll for in advance. Use Pattern A
            # (wait for a change, then require it settles) since navigating
            # from the ENTER ZN NUM prompt always produces a real display
            # change here.
            update = await self._send_and_wait(
                f"{zz}*",
                lambda u: u.alpha_text.startswith("Zn ZT"),
                settle=SETTLE_SECONDS,
            )
            code = parse_zone_type_summary(update.alpha_text)
            zt = describe(code)
            results[zone].zone_type_code = code
            results[zone].zone_type_label = zt.label
            results[zone].raw_summary = update.alpha_text
            yield ScanProgress(stage="types", zone=zone, total_zones=len(zones))
            yield results[zone]
            await self.transport.send_keys(self.partition, "#")  # back to ENTER ZN NUM

        await self.transport.send_keys(self.partition, "00")  # exit *56 to main installer menu

    # ---- *82 alpha descriptor walk (notes section 7-8) ----

    async def _walk_82(self, zones: list[int], results: dict[int, ZoneResult]):
        # Entering: *82, "PROGRAM ALPHA?" -> 1, "CUSTOM WORDS?" -> 0.
        # Correction (round 1): these two are bare digits, NO trailing */#.
        # Correction (round 4): "CUSTOM WORDS? -> 0" lands directly on zone
        # 1's edit view already -- if zone 1 is in the scan, don't wait for
        # a *change* here, use Pattern B for every per-zone read below.
        await self._send_and_wait("*82", lambda u: True)
        await self._send_and_wait("1", lambda u: True)
        await self._send_and_wait("0", lambda u: True)

        for zone in zones:
            zz = f"{zone:02d}"
            expected_header = f"* Zn {zz}"
            update = await self._send_and_wait(
                f"*{zz}",
                lambda u, h=expected_header: u.alpha_text.startswith(h),
                settle=0,  # Pattern B -- see round-4 correction above
            )
            name = parse_zone_descriptor(update.alpha_text)
            results[zone].name = name or None
            results[zone].raw_descriptor = update.alpha_text
            yield ScanProgress(stage="names", zone=zone, total_zones=len(zones))
            yield results[zone]

            # "save" -- mechanically re-commits the unchanged descriptor
            # (safety rule 3). Correction (round 6): don't wait for a
            # display change here, it's redundant with the next zone's
            # own header-match poll and wastes seconds/zone for nothing.
            await self.transport.send_keys(self.partition, "8")
            await asyncio.sleep(POST_SAVE_PACING_SECONDS)

        await self.transport.send_keys(self.partition, "*00")
        await self.transport.send_keys(self.partition, "0")
