# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Differential keystroke fuzzing: old build vs new build.

Runs the same seeded key sequences through two difffuzz binaries built
by refcheck/build.sh and compares their per-key state digests.  Nothing
here interprets the code: a finding is a replayable difference, and the
verdict is the replay.

Per seed:
  1. old and new each run once.  Identical traces: clean.
  2. Otherwise old runs again.  If the two old runs differ, the seed is
     nondeterministic on the old build and is set aside (counted, not
     reported as a finding).
  3. Otherwise new runs again.  If the two new runs differ, the finding
     is "new build nondeterministic"; if not, it is a divergence.

Findings are grouped by signature (kind + the state fields that differ
at the first divergent key).  One witness per group is shrunk by greedy
chunked key deletion that keeps the signature, then dumped readably at
the divergent key on both builds.

Usage (normally called by refcheck/run.py):
  difffuzz.py OLD_BIN NEW_BIN --seeds 1:500 --jobs 4 --out findings.json
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

import hunks as hk
import textio
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
SANDBOX = os.path.join(HERE, "sandbox.sh")
FIELDS = ["buf", "cur", "mark", "status", "kill", "reg", "win", "files", "lead",
          "screen"]
TIMEOUT = 30

ENV_EXTRA = {
    "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:exitcode=99",
    "UBSAN_OPTIONS": "print_stacktrace=1",
    "LC_ALL": "C.UTF-8",
    "TZ": "UTC",
}


def _env(sandbox):
    env = dict(os.environ)
    env.update(ENV_EXTRA)
    env["HOME"] = sandbox
    return env


def run_driver(binary, mode_args, sandbox):
    """Run binary in a fresh sandbox; return (fd3 text, status, stderr)."""
    # Always absolute: HOME is the sandbox, and a relative HOME changes
    # what "~" expands to (and once reached a cleanPath overflow).
    sandbox = os.path.abspath(sandbox)
    binary = os.path.abspath(binary)
    subprocess.run([SANDBOX, sandbox], check=True)
    fd, outpath = tempfile.mkstemp(prefix="df-out-")
    os.close(fd)
    try:
        # fd 3 carries the trace; a shell sets it up so that Python's
        # close_fds cannot take it away.
        cmd = ['sh', '-c', 'exec "$0" "$@" 3>"$DF_OUT"', binary] + mode_args
        env = _env(sandbox)
        env["DF_OUT"] = outpath
        try:
            p = subprocess.run(cmd, cwd=sandbox, env=env,
                               stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=TIMEOUT)
            rc = p.returncode
            err = textio.decode(p.stderr)
        except subprocess.TimeoutExpired as e:
            rc = "timeout"
            err = textio.decode(e.stderr or b"")
        text = textio.read(outpath)
    finally:
        os.unlink(outpath)
    if rc == 0:
        status = "ok"
    elif rc == "timeout" or rc == 3:
        status = "hang"
    elif rc == 99 or "runtime error:" in err or "AddressSanitizer" in err:
        status = "sanitizer"
    else:
        status = "crash:%s" % rc
    return text, status, err


def parse_trace(text):
    rows = []
    for line in text.splitlines():
        if line == "end" or line.startswith("skip "):
            continue
        parts = line.split()
        if not parts:
            continue
        idx = int(parts[0])
        fields = dict(p.split("=", 1) for p in parts[1:])
        rows.append((idx, fields))
    return rows


class Run:
    def __init__(self, binary, seed, skipfile, sandbox):
        text, self.status, self.err = run_driver(
            binary, ["trace", str(seed),
                     os.path.abspath(skipfile) if skipfile else "-"], sandbox
        )
        self.rows = parse_trace(text)
        self.skipped = None
        for line in text.splitlines()[:1]:
            if line.startswith("skip "):
                self.skipped = int(line.split()[1])

    def same_as(self, other):
        return self.status == other.status and self.rows == other.rows


def first_divergence(a, b):
    """(kind, fields, key index) for the first difference, or None."""
    for (ia, fa), (ib, fb) in zip(a.rows, b.rows):
        if ia != ib:
            return ("keys-consumed", ("cur",), min(ia, ib))
        diff = tuple(f for f in FIELDS if fa.get(f) != fb.get(f))
        if diff:
            return ("divergence", diff, ia)
    if len(a.rows) != len(b.rows) or a.status != b.status:
        at = min(len(a.rows), len(b.rows))
        idx = (a.rows[at - 1][0] if at else -1)
        if a.status != b.status:
            return ("new-" + b.status if b.status != "ok" else
                    "old-" + a.status, (), idx)
        return ("length", (), idx)
    return None


def signature(div):
    kind, fields, _ = div
    return kind + ":" + ",".join(fields)


def sanitizer_frame(err):
    """First user frame of a sanitizer report, for crash signatures."""
    for line in err.splitlines():
        if "runtime error:" in line:
            return line.strip()[:200]
    for line in err.splitlines():
        s = line.strip()
        if s.startswith("#") and " in " in s and "libc" not in s:
            fn = s.split(" in ", 1)[1].split()[0]
            if not fn.startswith(("__", "mem", "str")):
                return s[:200]
    return ""


def check_seed(args):
    old_bin, new_bin, seed, work = args
    sb = os.path.join(work, "sb-%d" % os.getpid())
    o1 = Run(old_bin, seed, None, sb)
    n1 = Run(new_bin, seed, None, sb)
    if o1.status != "ok" and not o1.rows:
        # The old build did not run the sequence at all, so nothing was
        # compared.  Never let that count as clean.
        return {"seed": seed, "result": "incomplete",
                "detail": (o1.status + ": " + o1.err.strip()[:200])}
    if o1.same_as(n1):
        if o1.status != "ok":
            return {"seed": seed, "result": "old-" + o1.status + "-same",
                    "detail": sanitizer_frame(o1.err)}
        return {"seed": seed, "result": "clean"}
    o2 = Run(old_bin, seed, None, sb)
    if not o1.same_as(o2):
        d = first_divergence(o1, o2)
        return {"seed": seed, "result": "old-nondeterministic",
                "fields": list(d[1]) if d else []}
    n2 = Run(new_bin, seed, None, sb)
    if not n1.same_as(n2):
        d = first_divergence(n1, n2)
        return {"seed": seed, "result": "finding",
                "signature": "new-nondeterministic:" + ",".join(d[1] if d else ()),
                "index": d[2] if d else -1}
    d = first_divergence(o1, n1)
    res = {"seed": seed, "result": "finding", "signature": signature(d),
           "index": d[2]}
    if n1.status == "sanitizer":
        res["detail"] = sanitizer_frame(n1.err)
    return res


def nkeys_of(binary, seed, sb):
    p = textio.run([binary, "keys", str(seed)], env=_env(sb), timeout=TIMEOUT)
    lines = p.stdout.splitlines()
    screen = lines[0].split()[1]
    names = [l.split(" ", 1)[1] for l in lines[1:]]
    return screen, names


class SkipNotApplied(Exception):
    pass


def minimise(old_bin, new_bin, seed, want, work, max_probes=400):
    """Greedy chunked key deletion keeping the signature `want`."""
    sb = os.path.join(work, "sb-min")
    screen, names = nkeys_of(new_bin, seed, sb)
    total = len(names)
    skip = set()
    skipfile = os.path.join(work, "skip-%d" % seed)
    probes = 0

    def write(s):
        textio.write(skipfile, "".join("%d\n" % i for i in sorted(s)))

    def still_fails(s):
        nonlocal probes
        probes += 1
        write(s)
        o = Run(old_bin, seed, skipfile, sb)
        n = Run(new_bin, seed, skipfile, sb)
        if o.skipped != len(s) or n.skipped != len(s):
            raise SkipNotApplied("seed %d: wrote %d skips, drivers applied %s/%s"
                                 % (seed, len(s), o.skipped, n.skipped))
        if o.same_as(n):
            return False
        d = first_divergence(o, n)
        return d is not None and signature(d) == want

    chunk = max(1, total // 2)
    while chunk >= 1 and probes < max_probes:
        start = 0
        while start < total and probes < max_probes:
            block = set(range(start, min(start + chunk, total))) - skip
            if block and still_fails(skip | block):
                skip |= block
            start += chunk
        chunk //= 2
    write(skip)
    o = Run(old_bin, seed, skipfile, sb)
    n = Run(new_bin, seed, skipfile, sb)
    d = first_divergence(o, n)
    at = d[2] if d else -1
    kept_idx = [i for i in range(total) if i not in skip and i < at]
    kept = [names[i] for i in kept_idx]
    return {"skipfile": skipfile, "skip": sorted(skip), "screen": screen,
            "keys": kept, "kept_indices": kept_idx, "index": at,
            "probes": probes,
            "confirmed": d is not None and signature(d) == want}


def dump(binary, seed, skipfile, at, work):
    sb = os.path.join(work, "sb-dump")
    text, status, err = run_driver(
        binary, ["dump", str(seed), os.path.abspath(skipfile) if skipfile else "-",
                 str(at)], sb)
    sections, cur = {}, None
    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1]
            sections[cur] = []
        elif cur:
            sections[cur].append(line)
    return {k: "\n".join(v) for k, v in sections.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old_bin")
    ap.add_argument("new_bin")
    ap.add_argument("--seeds", default="1:200")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--budget", type=float, default=0,
                    help="stop starting new seeds after this many seconds")
    ap.add_argument("--work", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hunks", help="JSON list of changed hunks (hunks.py)")
    ap.add_argument("--cov-bin", help="coverage build of the new commit")
    ap.add_argument("--cov-obj", help="its object directory")
    ap.add_argument("--cov-src", help="its source directory")
    ap.add_argument("--cov-seeds", type=int, default=300,
                    help="seeds run on the coverage build to see which "
                         "changed hunks the fuzzer reaches")
    ap.add_argument("--max-groups", type=int, default=10,
                    help="minimise at most this many signature groups")
    a = ap.parse_args()

    first, last = (int(x) for x in a.seeds.split(":"))
    # Absolute: the drivers run inside their sandboxes and read skip
    # files from here.
    work = os.path.abspath(a.work or tempfile.mkdtemp(prefix="refcheck-df-"))
    os.makedirs(work, exist_ok=True)
    old_bin, new_bin = os.path.abspath(a.old_bin), os.path.abspath(a.new_bin)

    t0 = time.time()
    results = []
    seeds = list(range(first, last + 1))
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        batch = a.jobs * 4
        for i in range(0, len(seeds), batch):
            if a.budget and time.time() - t0 > a.budget:
                break
            chunk = [(old_bin, new_bin, s, work) for s in seeds[i:i + batch]]
            results.extend(ex.map(check_seed, chunk))
    run_secs = time.time() - t0

    groups = {}
    for r in results:
        if r["result"] == "finding":
            groups.setdefault(r["signature"], []).append(r)

    changed = textio.load_json(a.hunks) if a.hunks else []
    changed_files = sorted({h["file"] for h in changed})

    def locate(seed, skipfile, at):
        """Changed hunks the new build executes up to the divergence."""
        if not (a.cov_bin and changed):
            return []
        hk.clear_counts(a.cov_obj)
        dump(a.cov_bin, seed, skipfile, at, work)
        executed = hk.executed_lines(a.cov_obj, a.cov_src, changed_files)
        return hk.hit_hunks(changed, executed)

    findings = []
    failed = []
    ordered = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    for sig, rs in ordered[:a.max_groups]:
        rep = min(rs, key=lambda r: r["seed"])
        try:
            w = minimise(old_bin, new_bin, rep["seed"], sig, work)
        except SkipNotApplied as e:
            failed.append({"signature": sig, "seeds": [r["seed"] for r in rs],
                           "reason": "witness could not be shrunk: %s" % e})
            continue
        if not w["kept_indices"]:
            w["confirmed"] = False
        old_d = dump(old_bin, rep["seed"], w["skipfile"], w["index"], work)
        new_d = dump(new_bin, rep["seed"], w["skipfile"], w["index"], work)
        init = dump(old_bin, rep["seed"], w["skipfile"], -1, work)
        differing = [k for k in old_d if old_d.get(k) != new_d.get(k)]
        skip_text = textio.read(w["skipfile"])
        findings.append({
            "signature": sig,
            "seeds": [r["seed"] for r in rs],
            "seed": rep["seed"],
            "detail": rep.get("detail", ""),
            "location": locate(rep["seed"], w["skipfile"], w["index"]),
            "witness": {k: w[k] for k in ("screen", "keys", "kept_indices",
                                          "index", "probes", "confirmed")},
            "skip": skip_text,
            "initial": {"buf": init.get("buf", ""), "cur": init.get("cur", "")},
            "old": {k: old_d.get(k, "") for k in differing},
            "new": {k: new_d.get(k, "") for k in differing},
        })
    unminimised = failed + [
        {"signature": sig, "seeds": [r["seed"] for r in rs],
         "reason": "not minimised (group limit)"}
        for sig, rs in ordered[a.max_groups:]]

    # One root cause shows up as several signatures (the same bad hunk
    # reaches the kill ring in one sequence, a register in another).
    # Merge findings whose witnesses ran the same changed hunks, keeping
    # the shortest witness as the representative.
    # Witnesses whose changed lines are equal, or one a subset of the
    # other, are the same cause reached along different paths (one also
    # runs the branch next to the change).  Disjoint sets stay apart.
    groups_out = []  # [set(location), representative finding]
    for f in sorted(findings, key=lambda f: len(f["location"])):
        loc = set(f["location"])
        f["signatures"] = [f["signature"]]
        home = None
        if loc:
            for g in groups_out:
                if g[0] and (loc <= g[0] or g[0] <= loc):
                    home = g
                    break
        if home is None:
            groups_out.append([loc, f])
            continue
        m = home[1]
        home[0] |= loc
        sigs = m["signatures"] + [f["signature"]]
        seeds = sorted(set(m["seeds"]) | set(f["seeds"]))
        if len(f["witness"]["keys"]) < len(m["witness"]["keys"]):
            home[1] = m = f
        m["signatures"], m["seeds"] = sigs, seeds
    findings = [g[1] for g in groups_out]

    # Which changed hunks did the fuzzer execute at all?  Silence about a
    # hunk it never ran says nothing about that hunk.
    unreached, fuzz_counts = {}, {}
    if a.cov_bin and changed:
        hk.clear_counts(a.cov_obj)
        sb = os.path.join(work, "sb-cov")
        for s in [r["seed"] for r in results][:a.cov_seeds]:
            run_driver(a.cov_bin, ["trace", str(s), "-"], sb)
        cov = hk.line_counts(a.cov_obj, a.cov_src, changed_files)
        fuzz_counts = cov
        unreached = hk.unrun_changed_lines(changed, cov)

    counts = {}
    for r in results:
        counts[r["result"]] = counts.get(r["result"], 0) + 1
    out = {
        "seeds_run": len(results),
        "seconds": round(run_secs, 1),
        "counts": counts,
        "old_nondeterministic_fields": sorted({f for r in results
                                               if r["result"] == "old-nondeterministic"
                                               for f in r["fields"]}),
        "findings": findings,
        "unminimised": unminimised,
        "seeds_by_result": {k: [r["seed"] for r in results if r["result"] == k]
                            for k in sorted({r["result"] for r in results})
                            if k not in ("clean", "finding")},
        "unreached_hunks": unreached,
        "fuzz_counts": fuzz_counts,
        "cov_seeds": min(a.cov_seeds, len(results)) if a.cov_bin else 0,
    }
    textio.dump_json(out, a.out, indent=1)
    print("difffuzz: %d seeds in %.0fs: %s; %d finding group(s)"
          % (len(results), run_secs, counts, len(findings)))


if __name__ == "__main__":
    sys.exit(main())
