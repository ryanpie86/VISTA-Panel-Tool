"""Persistence for named sites -- lets a tech save a scan under a site name
and reload it on a later visit instead of starting over.

Deliberately does NOT persist the installer code: a saved site is scan
*results* (what the panel is currently programmed with), not panel
credentials. The tech re-enters the code each visit to talk to the panel
live; loading a site only restores what was already read from it.

Storage is a single JSON file rather than a database -- this runs on one
Pi, for one tech's toolkit, and the whole "sites" collection is small
enough that a database would be pure overhead.
"""

from __future__ import annotations

import json
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

DATA_DIR = Path(os.environ.get("VISTA_DATA_DIR", Path.home() / ".vista_tool"))
SITES_FILE = DATA_DIR / "sites.json"


def _load_all() -> dict[str, Any]:
    if not SITES_FILE.exists():
        return {}
    return json.loads(SITES_FILE.read_text())


def _save_all(data: dict[str, Any]) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    SITES_FILE.write_text(json.dumps(data, indent=2))


def list_sites() -> list[dict[str, Any]]:
    """Summary of every saved site, most recently saved first."""
    data = _load_all()
    summaries = [
        {
            "name": name,
            "panel_type": site["panel_type"],
            "saved_at": site["saved_at"],
            "zone_count": len(site.get("zones", [])),
        }
        for name, site in data.items()
    ]
    summaries.sort(key=lambda s: s["saved_at"], reverse=True)
    return summaries


def save_site(name: str, panel_type: str, zones: list[dict[str, Any]]) -> None:
    if not name.strip():
        raise ValueError("Site name is required")
    data = _load_all()
    data[name] = {
        "panel_type": panel_type,
        "zones": zones,
        "saved_at": datetime.now(timezone.utc).isoformat(),
    }
    _save_all(data)


def get_site(name: str) -> dict[str, Any] | None:
    return _load_all().get(name)
