#!/usr/bin/env python3
"""Catch mod config vars whose name is RESERVED BY THE HOST.

The mod loader gives every discovered mod a bool at `mod.<escaped id>.enabled` for the mod
manager's own on/off checkbox, created before any mod initializes. `ConfigService::register_var`
formats a mod's own var to `mod.<escaped id>.<name>` in exactly the same namespace, so a mod that
registers a var literally named "enabled" collides with its own manager entry: register_var
returns MOD_CONFLICT and the mod dies at that registration.

It is silent in every way that matters. The tree builds, the mod packages, the manifest is fine,
and the only symptom is a runtime line naming whatever the mod called its own option -- which reads
like a mod bug rather than a name collision. Celestial Orbit shipped that way and simply never
loaded.

This check runs offline against `mods/*/src/mod.cpp`. When the fetched game tree is present it also
RE-DERIVES the reserved list from `loader.cpp` instead of trusting the constant below, so a host
that reserves a second name is reported rather than silently missed. Skips cleanly with no tree.

Usage:  python3 tools/check_reserved_config_names.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_TREE = ROOT / "dusklight"

# Fallback when the game tree is not fetched. Verified against the tree when it is.
KNOWN_RESERVED = {"enabled"}

# static std::string mod_enabled_cvar_name(...) { return fmt::format("mod.{}.enabled", ...); }
RESERVED_IN_HOST = re.compile(r'fmt::format\(\s*"mod\.\{\}\.([A-Za-z0-9_-]+)"')
# desc.name = "foo";  /  fooDesc.name = "foo";
VAR_NAME_ASSIGN = re.compile(r'^\s*\w*[Dd]esc\.name\s*=\s*"([^"]+)"\s*;', re.MULTILINE)
# register_bool_option("foo", ...) / register_int_option("foo", ...) and friends
VAR_NAME_HELPER = re.compile(r'\bregister_(?:bool|int|float|string)(?:_option)?\(\s*"([^"]+)"')


def host_reserved_names() -> tuple[set[str], bool]:
    """Reserved names re-derived from the game tree, plus whether the tree was readable."""
    loader = GAME_TREE / "src/dusk/mods/loader/loader.cpp"
    if not loader.is_file():
        return KNOWN_RESERVED, False
    found = set(RESERVED_IN_HOST.findall(loader.read_text(encoding="utf-8", errors="replace")))
    return (found or KNOWN_RESERVED), True


def main() -> int:
    reserved, from_tree = host_reserved_names()

    if from_tree and reserved != KNOWN_RESERVED:
        # Not a failure by itself, but the constant above is now stale and the message below
        # would name the wrong thing.
        print(
            f"NOTE: host reserves {sorted(reserved)}, this script's fallback says "
            f"{sorted(KNOWN_RESERVED)} -- update KNOWN_RESERVED.",
            file=sys.stderr,
        )

    failures: list[str] = []
    scanned = 0
    for source in sorted((ROOT / "mods").glob("*/src/mod.cpp")):
        text = source.read_text(encoding="utf-8", errors="replace")
        scanned += 1
        names = set(VAR_NAME_ASSIGN.findall(text)) | set(VAR_NAME_HELPER.findall(text))
        for name in sorted(names & reserved):
            failures.append(
                f"{source.relative_to(ROOT)}: config var \"{name}\" is RESERVED by the host "
                f"(the mod manager's own mod.<id>.{name} toggle). register_var will return "
                f"MOD_CONFLICT and the mod will fail to load. Rename it."
            )

    origin = "re-derived from the fetched game tree" if from_tree else "fallback list (tree absent)"
    if failures:
        print("\n".join(failures), file=sys.stderr)
        print(f"\n{len(failures)} reserved config name(s); reserved = {sorted(reserved)} [{origin}]",
              file=sys.stderr)
        return 1

    print(f"reserved config names: {scanned} mod(s) clean; reserved = {sorted(reserved)} [{origin}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
