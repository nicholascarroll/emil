# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Changed lines between two commits, and which of them a run executed.

A hunk is identified by (file, first_line, last_line) on the new side of
`git diff -U0`.  A pure deletion has no new-side lines; it is recorded
as the new-side line it sits after, and counts as executed if that line
or the next one ran.
"""

import glob
import gzip
import json
import os
import re
import subprocess

import textio

HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def changed_hunks(repo, old, new, pattern="*.c"):
    out = textio.run(
        ["git", "-C", repo, "diff", "-U0", "--no-color", old, new, "--",
         pattern], errors=textio.KEEP, check=True).stdout
    hunks, path = [], None
    for line in out.splitlines():
        if line.startswith("+++ "):
            p = line[4:]
            path = None if p == "/dev/null" else p[2:]
        m = HUNK_RE.match(line)
        if m and path and not path.startswith("refcheck/"):
            start = int(m.group(1))
            count = int(m.group(2)) if m.group(2) is not None else 1
            if count == 0:
                hunks.append({"file": path, "start": start, "end": start,
                              "deletion": True})
            else:
                hunks.append({"file": path, "start": start,
                              "end": start + count - 1, "deletion": False})
    return hunks


def hunk_id(h):
    return "%s:%d-%d" % (h["file"], h["start"], h["end"])


def line_counts(objdir, srcdir, files):
    """{file: {line: count}} for every executable line, per gcov."""
    result = {}
    tmp = os.path.join(objdir, "gcov-out")
    os.makedirs(tmp, exist_ok=True)
    for f in files:
        src = os.path.join(srcdir, f)
        if not os.path.exists(src):
            continue
        for old in glob.glob(os.path.join(tmp, "*.gcov.json.gz")):
            os.unlink(old)
        subprocess.run(["gcov", "-j", "-o", objdir, src], cwd=tmp,
                       capture_output=True)
        counts = {}
        for g in glob.glob(os.path.join(tmp, "*.gcov.json.gz")):
            with gzip.open(g, "rt", encoding="utf-8", errors=textio.KEEP) as fh:
                data = json.load(fh)
            for fe in data.get("files", []):
                if os.path.basename(fe["file"]) != os.path.basename(f):
                    continue
                for ln in fe.get("lines", []):
                    n = ln["line_number"]
                    counts[n] = max(counts.get(n, 0), ln.get("count", 0))
        result[f] = counts
    return result


def executed_lines(objdir, srcdir, files):
    """{file: set(line)} of lines with a nonzero count, per gcov."""
    return {f: {n for n, c in cnt.items() if c > 0}
            for f, cnt in line_counts(objdir, srcdir, files).items()}


def unrun_changed_lines(hunks, counts):
    """{file: [line]} of executable changed lines with a zero count."""
    out = {}
    for h in hunks:
        if h["deletion"]:
            continue
        cnt = counts.get(h["file"], {})
        for n in range(h["start"], h["end"] + 1):
            if cnt.get(n) == 0:
                out.setdefault(h["file"], []).append(n)
    return out


def hit_hunks(hunks, executed):
    hit = []
    for h in hunks:
        lines = executed.get(h["file"], set())
        if h["deletion"]:
            if h["start"] in lines or h["start"] + 1 in lines:
                hit.append(hunk_id(h))
        elif any(l in lines for l in range(h["start"], h["end"] + 1)):
            hit.append(hunk_id(h))
    return hit


def clear_counts(objdir):
    for g in glob.glob(os.path.join(objdir, "*.gcda")):
        os.unlink(g)
