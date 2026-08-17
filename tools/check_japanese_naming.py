#!/usr/bin/env python3
"""Check that docs/japanese-naming.md only names game symbols that exist.

The glossary's whole value is that a mod session can trust it instead of guessing
at a romanized name. An entry naming a symbol the platform has since renamed or
dropped is worse than no entry at all: it is a wrong answer delivered with the
authority of a glossary, and a glossary is exactly the kind of document nobody
re-reads. The symbols are therefore machine-checked. The Japanese readings are
ours and cannot be checked by anything here.

This repo does not contain the game. cmake/FetchDusklight.cmake clones it into
./dusklight during a configure, so this script checks against that tree when it
is present and SKIPS - loudly, exit 0 - when it is not. A skip is the normal
state of a fresh clone; failing there would just train people to ignore it.

    python3 tools/check_japanese_naming.py
    DUSKLIGHT_DIR=/path/to/dusklight python3 tools/check_japanese_naming.py

The tree comes from automata-rtx/dusklight-ao at DUSKLIGHT_VERSION - the platform the mods
are actually built against, NOT stock upstream TwilitRealm/dusklight. Its game code is
upstream (the fork delta is renderer + SDK only), so the glossary checks out either way, but
the pinned SHA only resolves against the fork. Pointing DUSKLIGHT_DIR at a checkout of a
different base would check the glossary against source the mods are not built against, which
is worse than skipping. docs/japanese-naming.md 2.1 has the exact clone commands.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOC = "docs/japanese-naming.md"

# Where the game code lives inside the fetched tree.
HAYSTACKS = ["src", "include", "libs"]

# Backticked tokens in the document that are prose, our own code, or game data
# spelled in a form no C identifier carries (material-name suffixes, the MAnn
# codes). Kept short and explicit: a token silently exempted is a glossary entry
# that stops being checked.
NON_SYMBOLS = {
    "camelCase",
    "snake_case",
    "rg",
    "grep",
    "Grep",
    "locale",
    "TARGET_PC",
    "DEBUG",
    "MAnn",
    "MA00",
    "MA01",
    "MA03",
    "MA04",
    "MA06",
    "MA09",
    "MA16",
    "MA17",
    "MA19",
    "_Gake",
    "_Kusa",
    "_Nami",
    "_Mera",
    "_Kasan",
    "_Mizugiwa",
    "_Word",
    "Word",
    "minami",
    "nami",
    "ese",
    "nama",
    "kytag",
    "moya",
    "vrkumo",
    "kankyo",
    "kumo",
    "kasumi",
    "kage",
    "wether",
    "DUSKLIGHT_VERSION",
    "DUSKLIGHT_DIR",
    # A full material name. These live in .bmd asset data, not in the source
    # tree, so nothing here can confirm one - which is why the document says so
    # rather than asserting it.
    "cc_MA06_NigoriWater_v_x",
}

# Our own code and build knobs; not game symbols.
OURS_PREFIXES = ("er_", "hub_", "mods/", "docs/", "tools/", "cmake/")

FILE_SUFFIXES = (".cpp", ".h", ".inc")

failures: list[str] = []


def fail(message: str) -> None:
    failures.append(message)


def game_tree() -> Path | None:
    override = os.environ.get("DUSKLIGHT_DIR")
    candidate = Path(override) if override else REPO / "dusklight"
    if not candidate.is_absolute():
        candidate = (REPO / candidate).resolve()
    return candidate if (candidate / "src").is_dir() else None


def tokens_from_doc(text: str) -> tuple[list[str], list[str]]:
    """Split backticked tokens into identifiers and filenames.

    They need different checks: an identifier has to appear in some file's text,
    a filename has to *be* a file. Searching a filename as content only works by
    accident, when a header comment happens to repeat it.
    """
    idents: list[str] = []
    files: list[str] = []
    for token in dict.fromkeys(re.findall(r"`([^`\n]+)`", text)):
        if token in NON_SYMBOLS or token.startswith(OURS_PREFIXES):
            continue
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token):
            idents.append(token)
        elif token.endswith(FILE_SUFFIXES) and re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*\.[a-z]+", token
        ):
            files.append(token)
    return idents, files


def main() -> int:
    path = REPO / DOC
    if not path.is_file():
        print(f"{DOC} missing - it is this repo's naming reference", file=sys.stderr)
        return 1

    idents, files = tokens_from_doc(path.read_text(encoding="utf-8", errors="replace"))
    if not idents:
        print(f"{DOC} names no checkable symbols - has the format changed?", file=sys.stderr)
        return 1

    tree = game_tree()
    if tree is None:
        print(
            f"{DOC}: SKIPPED - no game tree.\n"
            "  Run a CMake configure (which fetches ./dusklight), or set DUSKLIGHT_DIR\n"
            f"  to an existing checkout. {len(idents)} symbols and {len(files)} files went unchecked."
        )
        return 0

    present = [d for d in HAYSTACKS if (tree / d).is_dir()]

    for token in idents:
        proc = subprocess.run(
            ["rg", "--fixed-strings", "--quiet", "--", token, *present],
            cwd=tree,
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            fail(f"{DOC} names `{token}`, which is not in the game tree at {tree}")

    proc = subprocess.run(
        ["git", "-C", str(tree), "ls-files", *present], capture_output=True, text=True
    )
    basenames = (
        {Path(line).name for line in proc.stdout.splitlines() if line}
        if proc.returncode == 0
        else set()
    )
    for token in files:
        if basenames and token not in basenames:
            fail(f"{DOC} names the file `{token}`, which is not in the game tree at {tree}")

    if failures:
        print(f"{DOC}: {len(failures)} problem(s)\n", file=sys.stderr)
        for message in failures:
            print(f"  {message}", file=sys.stderr)
        return 1

    print(f"{DOC}: {len(idents) + len(files)} symbols checked against {tree}, all present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
