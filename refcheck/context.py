# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Change context: background on every function a commit touched.

Not findings: the report's findings stay interesting-only, and this is
appended as a collapsed section after them.  Everything here comes from
data the checks already computed: the clang call graph and footprints
(footprint check), test-suite coverage (coverage check) and fuzzer
coverage (difffuzz check).  A part whose check did not run says so.
"""

from collections import deque


def _name(k):
    return k.split("@")[0]


def _callers(fp):
    up = {}
    for k, f in fp.items():
        for callee, _, _ in f["calls"]:
            if callee in fp and callee != k:
                up.setdefault(callee, set()).add(k)
    return up


def _chain(up, k):
    """A shortest caller chain from a function nothing calls down to k."""
    prev, q, seen = {k: None}, deque([k]), {k}
    while q:
        x = q.popleft()
        if not up.get(x):
            path = [x]
            while prev[path[-1]] is not None:
                path.append(prev[path[-1]])
            return path
        for p in sorted(up[x]):
            if p not in seen:
                seen.add(p)
                prev[p] = x
                q.append(p)
    return [k]


def _cover(counts, f, start, end):
    if counts is None:
        return "not measured"
    cnt = counts.get(f["file"])
    if cnt is None:
        return "not measured"
    lines = [n for n in range(start, end + 1) if n in cnt]
    if not lines:
        return "no executable lines"
    run = sum(1 for n in lines if cnt[n] > 0)
    return "%d of %d lines" % (run, len(lines))


def _paths(total, i):
    ps = [p for p, v in total.items() if v[i]]
    named = sorted(p for p in ps if not p.startswith(("(", "$")))
    via_ptr = [p for p in ps if p.startswith(("(", "$"))]
    return named, via_ptr


def _list(xs, n=8):
    return ", ".join(xs[:n]) + (", ... (%d)" % len(xs) if len(xs) > n else "")


def render(c):
    old, new = c.fp_old, c.fp_new
    if new is None:
        return ("\n<details><summary>Change context</summary>\n\nNot available: "
                "it needs the footprint check, which was skipped.\n</details>\n")
    changed = sorted(k for k in set(old) & set(new) if old[k]["src"] != new[k]["src"])
    added = sorted(set(new) - set(old))
    removed = sorted(set(old) - set(new))
    if not (changed or added or removed):
        return ""
    up_new, up_old = _callers(new), _callers(old)
    blocks = []
    for k in changed + added:
        f = new[k]
        b = ["%s()  %s:%d-%d  (%s)" % (f["name"], f["file"], f["start"], f["end"],
                                      "changed" if k in old else "new")]
        chain = _chain(up_new, k)
        if len(chain) > 1:
            b.append("  reached from: " + " -> ".join(_name(x) for x in chain))
        callers = sorted(_name(x) for x in up_new.get(k, ()))
        b.append("  callers: " + (_list(callers) if callers else "none"))
        callees = []
        for callee, _, _ in f["calls"]:
            if callee in new and _name(callee) not in callees and callee != k:
                callees.append(_name(callee))
        b.append("  calls:   " + (_list(callees) if callees else "nothing in emil"))
        # Its own accesses: in emil nearly everything reaches nearly
        # everything through prompts and redraws, so the transitive set
        # of a high-level function is long and says little.
        for i, verb in ((1, "writes"), (0, "reads")):
            named, ptr = _paths(f["access"], i)
            tn, tp = _paths(f["total"], i)
            line = "  %-7s  %s" % (verb + ":", _list(named, 6) if named else "no global state itself")
            if ptr:
                line += "; %d field(s) through pointers" % len(ptr)
            line += "  (with callees: %d)" % (len(tn) + len(tp))
            b.append(line)
        b.append("  run by tests: %s · by the fuzzer: %s" % (
            _cover(c.test_counts or None, f, f["start"], f["end"]),
            _cover(c.fuzz_counts, f, f["start"], f["end"])))
        blocks.append("\n".join(b))
    for k in removed:
        f = old[k]
        callers = sorted(_name(x) for x in up_old.get(k, ()))
        blocks.append("%s()  %s:%d-%d  (removed)\n  had callers: %s" % (
            f["name"], f["file"], f["start"], f["end"],
            _list(callers) if callers else "none"))
    n = len(changed) + len(added) + len(removed)
    return ("\n<details><summary>Change context: %d function%s</summary>\n\n"
            "```text\n%s\n```\n</details>\n"
            % (n, "" if n == 1 else "s", "\n\n".join(blocks)))
