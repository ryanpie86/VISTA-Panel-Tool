# Regenerating these screenshots

These are captured against `tests/fake_transport.py` (the same scripted
fake panel the test suite uses), not real hardware — no Envisalink/RP2040
needed to update them.

1. Run the app with `build_transport()` swapped for a `FakeTransport`
   scripted to answer a handful of zones (type + name) instead of the real
   `tpi`/`rp2040` transports. A short responder keyed on the outgoing
   keystrokes is enough — see `test_56_walk_reads_two_zones` in
   `tests/test_zone_discovery.py` for the response-format reference.
2. Drive it with headless Chromium (Playwright) at a phone-sized viewport
   (~480x900) through: Home (fill installer code, e.g. via the backdoor
   button) → Next → Tools → Zone Discovery → Start Scan → wait for "Scan
   complete" → Back → Save Site → Back → Load previous site.
3. Screenshot each screen, clipped to its actual content height (not the
   full viewport) so the images don't carry dead space below the UI.
4. Overwrite the corresponding file in this directory (`home.png`,
   `tools.png`, `zone-discovery.png`, `load-site.png` — add new files for
   new screens as they're built) and update `README.md`'s "UI Features"
   section if the set of screens or their captions changed.

Do this in the same push as any UI-visible change (new/changed screen,
button, layout) so the README never shows a stale UI.
