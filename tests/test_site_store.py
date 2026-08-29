"""Tests for site_store.py -- the save/reload-a-site persistence used by
the "Save Site" / "Load previous site" UI flow."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pytest

from vista_tool import site_store


@pytest.fixture(autouse=True)
def isolated_store(tmp_path, monkeypatch):
    monkeypatch.setattr(site_store, "DATA_DIR", tmp_path)
    monkeypatch.setattr(site_store, "SITES_FILE", tmp_path / "sites.json")


def test_list_sites_empty_when_nothing_saved():
    assert site_store.list_sites() == []


def test_save_then_get_round_trips():
    zones = [{"zone": 1, "zone_type_code": "01", "zone_type_label": "Entry/Exit #1", "name": "FRONT DOOR"}]
    site_store.save_site("123 Main St", "20P", zones)

    site = site_store.get_site("123 Main St")
    assert site["panel_type"] == "20P"
    assert site["zones"] == zones
    assert "saved_at" in site


def test_get_missing_site_returns_none():
    assert site_store.get_site("nope") is None


def test_save_rejects_blank_name():
    with pytest.raises(ValueError):
        site_store.save_site("   ", "20P", [])


def test_list_sites_sorted_most_recent_first():
    site_store.save_site("Site A", "20P", [])
    site_store.save_site("Site B", "21IP", [])

    names = [s["name"] for s in site_store.list_sites()]
    assert names == ["Site B", "Site A"]


def test_save_site_overwrites_existing_entry():
    site_store.save_site("123 Main St", "20P", [{"zone": 1}])
    site_store.save_site("123 Main St", "20P", [{"zone": 1}, {"zone": 2}])

    sites = site_store.list_sites()
    assert len(sites) == 1
    assert sites[0]["zone_count"] == 2
