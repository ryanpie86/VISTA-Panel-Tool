"""Vista-20P zone type code table.

Source: VISTA_ZONE_DISCOVERY_PROTOCOL_NOTES.md section 9, cross-checked
against the 20P Programming Manual.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class ZoneType:
    code: str
    label: str
    kind: str  # suggested sensor "kind" for UI display purposes only
    note: str = ""


# kind values: contact, motion, safety, smoke, co, function, none
ZONE_TYPES: dict[str, ZoneType] = {
    "00": ZoneType("00", "Not Used", "none"),
    "01": ZoneType("01", "Entry/Exit #1", "contact"),
    "02": ZoneType("02", "Entry/Exit #2", "contact"),
    "03": ZoneType("03", "Perimeter", "contact"),
    "04": ZoneType("04", "Interior Follower", "motion"),
    "05": ZoneType("05", "Trouble Day/Alarm Night", "contact"),
    "06": ZoneType("06", "24-Hr Silent", "safety"),
    "07": ZoneType("07", "24-Hr Audible", "safety"),
    "08": ZoneType("08", "24-Hr Aux", "safety", "typically fires a custom PGM output, not a plain sensor"),
    "09": ZoneType("09", "Fire", "smoke"),
    "10": ZoneType("10", "Interior w/Delay", "motion", "same family as 04, with entry delay"),
    "11": ZoneType("11", "Not Used", "none"),
    "12": ZoneType("12", "Monitor Zone (24-Hr Monitor)", "contact"),
    "13": ZoneType("13", "Not Used", "none"),
    "14": ZoneType("14", "Carbon Monoxide", "co"),
    "15": ZoneType("15", "Not Used", "none"),
    "16": ZoneType("16", "Fire w/Verify", "smoke"),
    "17": ZoneType("17", "Not Used / reserved", "none", "installer cannot assign via *56"),
    "18": ZoneType("18", "Not Used / reserved", "none", "installer cannot assign via *56"),
    "19": ZoneType("19", "Not Used / reserved", "none", "installer cannot assign via *56"),
    "20": ZoneType("20", "Arm-STAY", "function", "keyswitch-style panel function, not a physical sensor"),
    "21": ZoneType("21", "Arm-AWAY", "function", "keyswitch-style panel function, not a physical sensor"),
    "22": ZoneType("22", "Disarm", "function", "keyswitch-style panel function, not a physical sensor"),
    "23": ZoneType("23", "No Alarm Response", "contact"),
    "24": ZoneType("24", "Silent Burglary", "contact"),
    "77": ZoneType("77", "Keyswitch", "contact"),
    "81": ZoneType("81", "AAV Monitor Zone", "contact", "not independently confirmed against real hardware"),
    "90": ZoneType("90", "Configurable", "contact", "not independently confirmed against real hardware"),
    "91": ZoneType("91", "Configurable", "contact", "not independently confirmed against real hardware"),
}


def describe(code: str) -> ZoneType:
    """Look up a zone type code, tolerating unknown codes."""
    code = code.strip().zfill(2)
    return ZONE_TYPES.get(code, ZoneType(code, f"Unknown ({code})", "none"))
