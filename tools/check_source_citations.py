#!/usr/bin/env python3
"""Verify the `file.cpp:LINE` citations in our docs still point where they claim.

Our documentation argues from the game's and aurora's source, and it cites line numbers:
"TP enables it globally (`d_kankyo.cpp:1257`)". Those numbers are evidence, and a re-platform
silently invalidates every one of them - the move to upstream Dusklight 2.0 moved
GXSetFogRangeAdj from d_kankyo.cpp:1257 to :9459, with nothing to signal it.

There is no recorded answer for "what was line 1257 supposed to be", so this script infers one:
a citation almost always sits next to the identifier it is evidence for, in backticks, in the
same sentence. So take the identifiers near each citation, and ask whether any of them appears
near the cited line.

    python3 tools/check_source_citations.py           # summary
    python3 tools/check_source_citations.py -v        # every citation, including OK ones
    DUSKLIGHT_DIR=/path/to/dusklight python3 tools/check_source_citations.py

Verdicts:
    OK      an anchor identifier appears within WINDOW lines of the citation.
    DRIFT   the anchor is in that file, but somewhere else. The suggested line is printed;
            it is a strong hint, not gospel - check it before editing.
    NOANCHOR  no identifier near the citation appears in the file at all. Usually means the
            prose names the concept rather than the symbol; read it yourself.
    RANGE   the file has fewer lines than the citation. Always a real break.
    NOFILE / AMBIGUOUS  the basename does not resolve to exactly one file in the tree.

Exit status is 1 only for RANGE and NOFILE, which cannot be anything but wrong. DRIFT is
reported loudly but does not fail: this runs on a tree that is fetched, not pinned to the doc,
and a advisory checker that cries wolf gets ignored - which is the failure mode it exists to
prevent. Read the report after any pin bump and fix what it names.

Skips cleanly when the game tree is not present, like check_japanese_naming.py.
"""

import os
import re
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREE = os.environ.get("DUSKLIGHT_DIR") or os.path.join(REPO, "dusklight")
WINDOW = 40  # lines either side of the citation an anchor may appear in

# Where citations live. Our own mod sources cite themselves (mod.cpp:NNN), which resolves inside
# the repo rather than the fetched tree.
DOC_GLOBS = ["CLAUDE.md", "README.md"]
DOC_DIRS = ["docs"]
SRC_DIRS = ["mods"]

CITATION = re.compile(r"`?([A-Za-z0-9_]+\.(?:cpp|h|hpp|inc)):(\d+)(?:-(\d+))?`?")
IDENT = re.compile(r"`([A-Za-z_][A-Za-z0-9_:]{2,})`")
# Identifiers that are never useful as anchors: too common, or they name our code not theirs.
STOPWORDS = {
    "true", "false", "null", "NULL", "nullptr", "int", "float", "bool", "void", "const",
    "return", "static", "struct", "class", "this", "size_t", "uint32_t", "main",
}


def find_files(root, exts):
    out = defaultdict(list)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "node_modules")]
        for name in filenames:
            if name.rsplit(".", 1)[-1] in exts:
                out[name].append(os.path.join(dirpath, name))
    return out


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read().splitlines()


def doc_paths():
    paths = []
    for rel in DOC_GLOBS:
        full = os.path.join(REPO, rel)
        if os.path.exists(full):
            paths.append(full)
    for rel in DOC_DIRS + SRC_DIRS:
        root = os.path.join(REPO, rel)
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build")]
            for name in filenames:
                if name.endswith((".md", ".cpp", ".h")):
                    paths.append(os.path.join(dirpath, name))
    return sorted(set(paths))


def anchors_near(text, start, end):
    """Backticked identifiers in the text surrounding a citation, nearest first."""
    lo = max(0, start - 400)
    hi = min(len(text), end + 200)
    found = []
    for match in IDENT.finditer(text, lo, hi):
        name = match.group(1)
        if name in STOPWORDS or "." in name:
            continue
        # Skip the citation's own filename.
        distance = min(abs(match.start() - start), abs(match.start() - end))
        found.append((distance, name))
    found.sort()
    seen, out = set(), []
    for _, name in found:
        base = name.split("::")[-1]
        if base not in seen:
            seen.add(base)
            out.append(base)
    return out[:6]




def current_pin():
    """DUSKLIGHT_VERSION from the top-level CMakeLists, so a stale allowlist can say so."""
    try:
        text = open(os.path.join(REPO, "CMakeLists.txt"), encoding="utf-8").read()
    except OSError:
        return ""
    match = re.search(r'set\(DUSKLIGHT_VERSION\s+"([0-9a-f]+)"', text)
    return match.group(1) if match else ""


def load_verified():
    """Citations someone checked by hand, and the pin they checked them against."""
    path = os.path.join(REPO, "tools", "source_citations_verified.txt")
    entries, pins = {}, set()
    if not os.path.exists(path):
        return entries, pins
    for raw in open(path, encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) >= 2:
            entries[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else "")
            pins.add(parts[1])
    return entries, pins


def in_same_block(lines, cited, name):
    """True when `name` appears above `cited` with no top-level block close between them."""
    pattern = re.compile(r"\b%s\b" % re.escape(name))
    i = min(cited, len(lines)) - 1
    while i >= 0:
        line = lines[i]
        if pattern.search(line):
            return True
        # A `}` or `};` in column 0 ends a top-level definition; anything above it is a
        # different function, so an anchor found there is not evidence about this line.
        if i < cited - 1 and re.match(r"^\}", line):
            return False
        i -= 1
    return False


def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    if not os.path.isdir(TREE):
        print("source citations: game tree not present at %s - skipped "
              "(run cmake -B build to fetch it)" % TREE)
        return 0

    index = find_files(TREE, {"cpp", "h", "hpp", "inc"})
    # Our own sources, so mod.cpp citations resolve.
    for name, paths in find_files(os.path.join(REPO, "mods"), {"cpp", "h"}).items():
        index[name].extend(paths)

    verified, pins = load_verified()
    pin = current_pin()

    counts = defaultdict(int)
    problems = []

    for doc in doc_paths():
        text = open(doc, "r", encoding="utf-8", errors="replace").read()
        rel_doc = os.path.relpath(doc, REPO)
        for match in CITATION.finditer(text):
            base, first, last = match.group(1), int(match.group(2)), match.group(3)
            line_no = int(last) if last else first
            where = "%s:%s" % (base, match.group(2) + ("-" + last if last else ""))
            doc_line = text.count("\n", 0, match.start()) + 1

            paths = index.get(base, [])
            if not paths:
                counts["NOFILE"] += 1
                problems.append(("NOFILE", rel_doc, doc_line, where, "no such file in the tree"))
                continue
            if len(set(paths)) > 1:
                # Prefer a unique non-test path before giving up.
                real = [p for p in set(paths) if "/tests/" not in p and "/test/" not in p]
                if len(real) > 1:
                    # A doc named after a mod that cites `mod.cpp` means THAT mod's mod.cpp.
                    stem = os.path.basename(rel_doc).split(".")[0]
                    owned = [p for p in real
                             if os.sep + "mods" + os.sep in p
                             and stem.startswith(os.path.basename(os.path.dirname(
                                 os.path.dirname(p))))]
                    if len(owned) == 1:
                        real = owned
                if len(real) != 1:
                    counts["AMBIGUOUS"] += 1
                    problems.append(("AMBIGUOUS", rel_doc, doc_line, where,
                                     "%d files share this name" % len(set(paths))))
                    continue
                paths = real

            lines = read_lines(paths[0])
            if line_no > len(lines):
                counts["RANGE"] += 1
                problems.append(("RANGE", rel_doc, doc_line, where,
                                 "file has only %d lines" % len(lines)))
                continue

            names = anchors_near(text, match.start(), match.end())
            if not names:
                counts["NOANCHOR"] += 1
                if verbose:
                    problems.append(("NOANCHOR", rel_doc, doc_line, where,
                                     lines[first - 1].strip()[:70]))
                continue

            lo = max(0, first - 1 - WINDOW)
            hi = min(len(lines), line_no + WINDOW)
            window = "\n".join(lines[lo:hi])
            hit = next((n for n in names if n in window), None)
            # A citation often points INSIDE the function it names, hundreds of lines past the
            # signature -- `d_kankyo.cpp:4429` is evidence about setLightTevColorType_MAJI_sub,
            # which opens at 4226. Widening WINDOW to cover that would weaken every other check,
            # so instead ask a structural question: is the citation in the same top-level block as
            # an occurrence of the anchor? A line starting at column 0 with `}` closes one.
            if not hit:
                hit = next((n for n in names if in_same_block(lines, first, n)), None)
            if hit:
                counts["OK"] += 1
                if verbose:
                    problems.append(("OK", rel_doc, doc_line, where, hit))
                continue

            # Report the occurrence NEAREST the citation, not the first in the file. For a
            # common identifier the first one is usually noise -- `mMoyaMode` appears 40 times,
            # and "it is at line 1737, not 4594" is a worse hint than the occurrence 150 lines
            # from where the doc pointed.
            elsewhere = None
            for name in names:
                pattern = re.compile(r"\b%s\b" % re.escape(name))
                hits = [i for i, line in enumerate(lines, 1) if pattern.search(line)]
                if hits:
                    nearest = min(hits, key=lambda i: abs(i - first))
                    elsewhere = (name, nearest, len(hits))
                    break
            if elsewhere and where in verified:
                entry_pin, description = verified[where]
                # An entry records that someone LOOKED, and at which tree. Against a different
                # tree it is a stale claim, not a standing exemption -- so it goes back to being
                # reported, with the hint, rather than silently suppressing a real drift.
                if pin and entry_pin[:7] != pin[:7]:
                    counts["STALE"] += 1
                    name, nearest, hits = elsewhere
                    problems.append(("STALE", rel_doc, doc_line, where,
                                     "verified against %s, tree is %s (%s); nearest `%s` is line %d"
                                     % (entry_pin, pin[:7], description, name, nearest)))
                    continue
                counts["VERIFIED"] += 1
                if verbose:
                    problems.append(("VERIFIED", rel_doc, doc_line, where, description))
                continue
            if elsewhere:
                counts["DRIFT"] += 1
                name, nearest, hits = elsewhere
                detail = "nearest `%s` is line %d (cited %d)" % (name, nearest, first)
                if hits > 1:
                    detail += ", %d occurrences" % hits
                problems.append(("DRIFT", rel_doc, doc_line, where, detail))
            else:
                counts["NOANCHOR"] += 1
                if verbose:
                    problems.append(("NOANCHOR", rel_doc, doc_line, where,
                                     "none of %s found" % ", ".join(names[:3])))

    order = {"RANGE": 0, "NOFILE": 1, "STALE": 2, "AMBIGUOUS": 3, "DRIFT": 4,
         "NOANCHOR": 5, "VERIFIED": 6, "OK": 7}
    problems.sort(key=lambda p: (order[p[0]], p[1], p[2]))
    for kind, doc, doc_line, where, detail in problems:
        print("%-9s %s:%d  %-28s %s" % (kind, doc, doc_line, where, detail))

    total = sum(counts.values())
    print("\nsource citations: %d checked against %s" % (total, TREE))
    print("  " + "  ".join("%s=%d" % (k, counts[k]) for k in
                           ("OK", "VERIFIED", "STALE", "DRIFT", "NOANCHOR", "AMBIGUOUS", "RANGE", "NOFILE")
                           if counts[k]))
    if counts["RANGE"] or counts["NOFILE"]:
        print("\nRANGE/NOFILE citations cannot be right - fix them.")
        return 1
    if counts["VERIFIED"]:
        print("  %d hand-verified in tools/source_citations_verified.txt (pin %s)"
              % (counts["VERIFIED"], pin[:7] or "?"))
    if counts["STALE"]:
        print("\n%d hand-verified citation(s) were checked against a DIFFERENT tree than the one\n"
              "pinned now. Re-read each in the current tree and update its pin column in\n"
              "tools/source_citations_verified.txt -- do not just bump the pin."
              % counts["STALE"])
    if counts["DRIFT"]:
        print("\nDRIFT is advisory: the suggested line is a hint from the nearest identifier,\n"
              "not a recorded answer. Check each before editing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
