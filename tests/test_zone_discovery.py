"""Regression tests mirroring the real-hardware corrections captured in
VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md -- especially the round-4 "already on
zone 1" bug and the round-6 wasted-wait bug, since both were only caught by
a full live run and are exactly the kind of thing that's easy to
reintroduce during a port.
"""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tests.fake_transport import FakeTransport
from vista_tool.zone_discovery import (
    ZoneDiscoveryWalk,
    ZoneResult,
    WalkAborted,
    parse_zone_type_summary,
    parse_zone_descriptor,
    compute_overall_timeout,
)


def test_parse_zone_type_summary_hardwired_header():
    text = "Zn ZT P RC HW:RT01 00 1 10 EL:1 "
    assert len(text) == 32
    assert parse_zone_type_summary(text) == "00"


def test_parse_zone_type_summary_fire_zone():
    # data row "09 09 1 10 EL:1 " -> second token "09"
    header = "Zn ZT P RC HW:RT"
    data_row = "09 09 1 10 EL:1 "
    text = header + data_row
    assert parse_zone_type_summary(text) == "09"


def test_parse_zone_descriptor_named():
    text = "* Zn 09  FRONT  DOOR            "
    assert parse_zone_descriptor(text) == "FRONT DOOR"


def test_parse_zone_descriptor_unprogrammed():
    text = "* Zn 01                         "
    assert parse_zone_descriptor(text) == ""


def _summary_for(zone_type_code: str) -> str:
    zz_field = zone_type_code
    return "Zn ZT P RC HW:RT" + f"{zz_field} 00 1 10 EL:1 "[: 16]


def run(coro):
    return asyncio.run(coro)


def test_56_walk_reads_two_zones():
    def responder(partition, keys):
        if keys.endswith("*") and keys[:-1].isdigit():
            zone = keys[:-1]
            # Data row format per notes section 6: "<rpt> <ZT> P RC ...".
            # Zone type code is the SECOND token, not the first.
            code = "01" if zone == "01" else "09"
            return "Zn ZT P RC HW:RT" + f"01 {code} 1 10 EL:1 "
        return "SOME OTHER SCREEN               "

    transport = FakeTransport(responder)
    walk = ZoneDiscoveryWalk(transport, installer_code="4112", partition=1)

    async def collect():
        results = {}
        async for item in walk.run(zones=[1, 2], read_types=True, read_names=False):
            if isinstance(item, ZoneResult):
                results[item.zone] = item
        return results

    results = run(collect())
    assert results[1].zone_type_code == "01"
    assert results[2].zone_type_code == "09"
    # *99 must always be sent on exit
    assert (1, "*99") in transport.sent_log


def test_82_walk_zone1_already_selected_round4_regression():
    """Round-4 correction: entering *82/1/0 lands DIRECTLY on zone 1's edit
    view with NO further display change. If the walk used Pattern A
    (wait-for-change) here it would time out. This test fails loudly if
    that regression is reintroduced."""

    zone1_text = "* Zn 01  FRONT  DOOR            "

    def responder(partition, keys):
        if keys == "0":
            # CUSTOM WORDS? -> 0 lands directly on zone 1, already-visible,
            # no NEW display event fires. Simulate that by returning None.
            return None
        if keys == "*01":
            return zone1_text
        return None

    transport = FakeTransport(responder)
    # Pre-seed the "already showing zone 1" state before *82 walk begins,
    # since in reality CUSTOM WORDS?->0 doesn't emit a fresh push message.
    transport._publish_initial = None

    walk = ZoneDiscoveryWalk(transport, installer_code="4112", partition=1)

    async def collect():
        results = {}
        async for item in walk.run(zones=[1], read_types=False, read_names=True):
            if isinstance(item, ZoneResult):
                results[item.zone] = item
        return results

    results = run(collect())
    assert results[1].name == "FRONT DOOR"


def test_wireless_enrollment_marker_aborts():
    def responder(partition, keys):
        if keys.endswith("*"):
            return "S/N ENTRY REQUIRED              "
        return None

    transport = FakeTransport(responder)
    walk = ZoneDiscoveryWalk(transport, installer_code="4112", partition=1)

    async def collect():
        stages = []
        async for item in walk.run(zones=[1], read_types=True, read_names=False):
            if hasattr(item, "stage"):
                stages.append(item.stage)
        return stages

    stages = run(collect())
    assert "error" in stages
    # even on abort, *99 must still have been sent (safety rule 6)
    assert (1, "*99") in transport.sent_log


def test_overall_timeout_scales_with_zone_count():
    small = compute_overall_timeout(5, read_names=False)
    large = compute_overall_timeout(64, read_names=True)
    assert large > small
    assert large > 64 * 3  # sanity: budget actually grows with zone count
