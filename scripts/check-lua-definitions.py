#!/usr/bin/env python3
"""Checks that every host function the plugin API registers has a definition in docs/specs/basalt-proxy.d.lua."""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
code = "".join(p.read_text() for p in (root / "proxy-plugins" / "src").glob("*.cpp"))
registered = set(re.findall(r'pushHostFunction\(\s*\w+,\s*"([^"]+)"', code))
registered = {name for name in registered if not name.endswith(".")}
definitions = (root / "docs" / "specs" / "basalt-proxy.d.lua").read_text()
defined = {name.lower() for name in re.findall(r"function\s+([A-Za-z_.:]+)\s*\(", definitions)}

missing = []
# The accessors behind `player.data.key` are metamethods, so they need the `data` field, not a function of their own.
metamethods = {name for name in registered if name.startswith("player.data.")}
if metamethods and not re.search(r"@field\s+data\b", definitions):
    missing.append("the data field of a player")
for name in sorted(registered - metamethods):
    candidates = {name.lower(), name.lower().replace("player.data.", "player.data:")}
    if name.startswith("player.") and not name.startswith("player.data."):
        candidates.add("player:" + name.split(".", 1)[1])
    if name in {"send", "connect", "kick", "cancel"}:
        candidates |= {f"{owner}:{name}" for owner in ("player", "consolesource", "handle")}
    if name.startswith("log."):
        candidates.add(name.lower())
    if not candidates & defined:
        missing.append(name)

for name in missing:
    print(f"no definition for {name}")
sys.exit(1 if missing else 0)
