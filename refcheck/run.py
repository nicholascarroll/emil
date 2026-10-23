# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""refcheck: compare a new commit of emil against an old one and report
only what changed.

Every check here is deterministic and model-free.  Each one compares
the new commit with the old commit, and reports only differences the
patch introduced.  A check that could not do its job is reported as
incomplete, never as clean.

Usage:
  python3 refcheck/run.py --old OLD --new NEW [--repo .] [--work DIR]
                          [--fuzz-seeds 2000] [--fuzz-budget 600]
                          [--jobs N] [--skip CHECK,...]

Writes DIR/report-<new>.md and DIR/report-<new>.json.
Checks: warnings, tests, newtests, stale, footprint, difffuzz, coverage.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hunks as hk  # noqa: E402
import context  # noqa: E402
import textio  # noqa: E402
import report  # noqa: E402

ALL_CHECKS = ["warnings", "tests", "newtests", "stale", "footprint", "difffuzz",
              "coverage"]


def sh(cmd, **kw):
    """Run a tool; its output is decoded as UTF-8, bad bytes shown as \\xNN
    (textio.py says why)."""
    return textio.run(cmd, **kw)


def rev(repo, ref):
    return sh(["git", "-C", repo, "rev-parse", ref], check=True).stdout.strip()


def short(sha):
    return sha[:7]


class Ctx:
    def __init__(self, a):
        self.repo = os.path.abspath(a.repo)
        self.old = rev(self.repo, a.old)
        self.new = rev(self.repo, a.new)
        self.work = os.path.abspath(a.work)
        self.jobs = a.jobs
        self.a = a
        os.makedirs(self.work, exist_ok=True)
        self.hunks = hk.changed_hunks(self.repo, self.old, self.new)
        self.fp_old = self.fp_new = None   # set by the footprint check
        self.test_counts, self.fuzz_counts = {}, None
        self.all_hunks = hk.changed_hunks(self.repo, self.old, self.new, ".")

    def target(self, sha):
        """Build sha with build.sh (cached per sha); return its dir."""
        out = os.path.join(self.work, sha)
        if not os.path.exists(os.path.join(out, "difffuzz")) or \
                os.path.getmtime(os.path.join(out, "difffuzz")) < \
                os.path.getmtime(os.path.join(HERE, "difffuzz.c")):
            p = sh([os.path.join(HERE, "build.sh"), self.repo, sha, out],
                   env=dict(os.environ, JOBS=str(self.jobs)))
            if p.returncode != 0:
                raise BuildFailed(sha, p.stderr[-3000:])
        return out

    def copy_tree(self, sha, name):
        """A fresh, writable copy of sha's sources for a check to dirty."""
        src = os.path.join(self.target(sha), "src")
        dst = os.path.join(self.work, sha, name)
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns(".git"))
        return dst


class BuildFailed(Exception):
    def __init__(self, sha, log):
        super().__init__(sha)
        self.sha, self.log = sha, log


def result(name, findings=None, incomplete=None, ran=1):
    return {"check": name, "findings": findings or [],
            "incomplete": incomplete or [], "ran": ran}


# ---- warnings ---------------------------------------------------------

WARN_RE = re.compile(r"^([^:\s]+\.[ch]):(\d+):\d+: (warning|error): (.*?)(?: \[(-W[^\]]+)\])?$")


def hal_warnings(tree):
    text = textio.read(os.path.join(tree, "Makefile")).replace("\\\n", " ")
    m = re.search(r"^HAL_WARNINGS\s*=\s*(.*)$", text, re.M)
    return m.group(1).split() if m else ["-Wall", "-Wextra"]


CONFIGS = {
    "default": ["-O2"],
    "minimal": ["-Os", "-DEMIL_DISABLE_SHELL"],
}


def compile_diagnostics(tree, flags):
    diags = []
    for f in sorted(os.listdir(tree)):
        if not f.endswith(".c"):
            continue
        p = sh(["gcc", "-std=c99", "-D_DEFAULT_SOURCE", "-D_BSD_SOURCE",
                '-DEMIL_VERSION="refcheck"', "-I", tree] + flags +
               ["-c", f, "-o", "/dev/null"], cwd=tree)
        for line in p.stderr.splitlines():
            m = WARN_RE.match(line)
            if m:
                diags.append({"file": m.group(1), "line": int(m.group(2)),
                              "kind": m.group(3), "msg": m.group(4),
                              "flag": m.group(5) or ""})
    return diags


def check_warnings(c):
    findings, incomplete = [], []
    by_key = {}  # one finding per new warning, listing the configs
    trees = {s: os.path.join(c.target(s), "src") for s in (c.old, c.new)}
    flags = hal_warnings(trees[c.new])
    for cfg, extra in CONFIGS.items():
        res = {}
        for s, tree in trees.items():
            cache = os.path.join(c.work, s, "diag-%s.json" % cfg)
            if os.path.exists(cache):
                res[s] = textio.load_json(cache)
            else:
                res[s] = compile_diagnostics(tree, flags + extra)
                textio.dump_json(res[s], cache)
        # Line numbers move, so compare by (file, kind, message, flag),
        # counting repeats.  Quotes are compared alike: a compiler not
        # given a UTF-8 locale quotes 'x' where one with it quotes ‘x’.
        def key(d):
            msg = d["msg"].replace("\u2018", "'").replace("\u2019", "'")
            return (d["file"], d["kind"], msg, d["flag"])
        old_count = {}
        for d in res[c.old]:
            old_count[key(d)] = old_count.get(key(d), 0) + 1
        seen = {}
        for d in res[c.new]:
            k = key(d)
            seen[k] = seen.get(k, 0) + 1
            if seen[k] > old_count.get(k, 0):
                fk = (d["file"], d["line"], d["kind"], d["msg"], d["flag"])
                if fk in by_key:
                    by_key[fk]["configs"].append(cfg)
                    continue
                by_key[fk] = {"d": d, "configs": [cfg], "extra": extra}
    for (f, line, kind, msg, flag), v in by_key.items():
        cfgs = v["configs"]
        findings.append({
            "location": "%s:%d" % (f, line),
            "title": "New compiler %s (%s build%s)" % (
                kind, " and ".join(cfgs), "s" if len(cfgs) > 1 else ""),
            "why": "always (warning delta against %s, HAL_WARNINGS)" % short(c.old),
            "body": "%s:%d: %s%s" % (f, line, msg, " [%s]" % flag if flag else ""),
            "replay": "gcc $(HAL_WARNINGS) %s -c %s" % (" ".join(v["extra"]), f),
            "rank": 1 if kind == "error" else 3,
        })
    return result("warnings", findings, incomplete, ran=len(CONFIGS))


# ---- test suite -------------------------------------------------------

SUITE_RE = re.compile(r"^\s{2}(\S+)\s+(PASS|FAIL|WARN)\b(.*)$")


def run_suite(tree, extra_env=None, log=None):
    env = dict(os.environ)
    env.update(extra_env or {})
    sh(["make", "-s", "clean"], cwd=tree)
    p = sh(["make", "test"] + (["CFLAGS=" + env["COV"], "LDFLAGS=" + env["COV"]]
                               if "COV" in env else []),
           cwd=tree, env=env, timeout=1800)
    out = p.stdout + p.stderr
    if log:
        textio.write(log, out)
    suites, current, context = {}, None, {}
    for line in out.splitlines():
        m = SUITE_RE.match(line)
        if m:
            current = m.group(1)
            suites[current] = m.group(2)
            context[current] = [line.strip()]
        elif current and line.startswith("    ") and suites.get(current) != "PASS":
            context[current].append(line.rstrip())
        if line.startswith("✗"):
            suites["invariant: " + line[1:].strip()] = "FAIL"
            context["invariant: " + line[1:].strip()] = [line]
    return suites, context, p.returncode


def suite_results(c, sha):
    cache = os.path.join(c.work, sha, "suites.json")
    if os.path.exists(cache):
        return textio.load_json(cache)
    tree = c.copy_tree(sha, "testtree")
    suites, context, rc = run_suite(tree, log=os.path.join(c.work, sha, "tests.log"))
    data = {"suites": suites, "context": context, "rc": rc}
    textio.dump_json(data, cache)
    return data


def check_tests(c):
    old = suite_results(c, c.old)
    new = suite_results(c, c.new)
    if not new["suites"]:
        return result("tests", incomplete=["test suite produced no results "
                                           "(see %s)" % os.path.join(c.work, c.new, "tests.log")])
    newly = [s for s, v in new["suites"].items()
             if v == "FAIL" and old["suites"].get(s) != "FAIL"]
    findings, incomplete = [], []
    if newly:
        # Rerun once: a failure that does not repeat is flaky, not a
        # finding.
        tree = c.copy_tree(c.new, "testtree2")
        again, ctx2, _ = run_suite(tree)
        for s in newly:
            if again.get(s) == "FAIL":
                findings.append({
                    "location": "tests: " + s,
                    "title": "Test suite now fails" if s in old["suites"]
                    else "New test suite fails",
                    "why": "always (test suite, old vs new)",
                    "body": "\n".join(new["context"].get(s, [])[:25]),
                    "replay": "make test",
                    "rank": 1,
                })
            else:
                incomplete.append("suite %s failed once and passed on rerun (flaky)" % s)
    return result("tests", findings, incomplete)


def check_newtests(c):
    """The new commit's tests, run against the old commit's code.

    The default claim is "no behaviour change", so a new or changed test
    that fails on the old code shows a behaviour difference the test
    suite can see."""
    changed_tests = sorted({h["file"] for h in c.all_hunks
                            if h["file"].startswith("tests/")})
    if not changed_tests:
        return result("newtests", ran=0)
    tree = c.copy_tree(c.old, "newtests")
    new_src = os.path.join(c.target(c.new), "src")
    for f in changed_tests:
        src = os.path.join(new_src, f)
        if os.path.exists(src):
            os.makedirs(os.path.dirname(os.path.join(tree, f)), exist_ok=True)
            shutil.copy2(src, os.path.join(tree, f))
    suites, context, _ = run_suite(tree, log=os.path.join(c.work, c.new, "newtests.log"))
    new = suite_results(c, c.new)
    findings, incomplete = [], []
    touched = {os.path.splitext(os.path.basename(f))[0].replace("test_", "")
               for f in changed_tests}
    for s, v in suites.items():
        if v == "FAIL" and new["suites"].get(s) == "PASS":
            ctx = context.get(s, [])
            if any("build" in l.lower() or "error:" in l for l in ctx[:3]) and \
                    len(ctx) <= 3:
                incomplete.append("suite %s: new tests do not build against "
                                  "the old code" % s)
                continue
            findings.append({
                "location": "tests: " + s,
                "title": "New or changed test fails on the old code",
                "why": "tests changed in this patch (%s)" % ", ".join(changed_tests),
                "body": "The test passes on the new commit and fails on "
                        "%s, so it detects a behaviour change.\n%s"
                        % (short(c.old), "\n".join(ctx[:20])),
                "replay": "copy the new tests/ into %s and make test" % short(c.old),
                "rank": 2,
            })
    for s in touched:
        if s not in suites and s in new["suites"]:
            incomplete.append("suite %s did not run against the old code" % s)
    return result("newtests", findings, incomplete)


# ---- stale references ------------------------------------------------

def defined_functions(objdir):
    p = sh(["nm", "--defined-only"] +
           [os.path.join(objdir, f) for f in sorted(os.listdir(objdir))
            if f.endswith(".o")])
    names = set()
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t"):
            names.add(parts[2])
    return names


TEXT_GLOBS = (".c", ".h", ".1", ".md", ".sh", ".txt")


def check_stale(c):
    old = defined_functions(os.path.join(c.target(c.old), "cov", "obj"))
    new = defined_functions(os.path.join(c.target(c.new), "cov", "obj"))
    removed = sorted(n for n in old - new if not n.startswith("_"))
    if not removed:
        return result("stale", ran=0)
    tree = os.path.join(c.target(c.new), "src")
    findings = []
    for name in removed:
        pat = re.compile(r"\b%s\b" % re.escape(name))
        hits = []
        for root, _, files in os.walk(tree):
            rel = os.path.relpath(root, tree)
            if "/.git" in root or rel.split(os.sep)[0] == "refcheck":
                continue
            for f in files:
                if not f.endswith(TEXT_GLOBS) or f == "CHANGELOG.md":
                    continue
                path = os.path.join(root, f)
                try:
                    lines = textio.read(path).splitlines()
                except OSError:
                    continue
                for i, l in enumerate(lines, 1):
                    if pat.search(l):
                        hits.append("%s:%d: %s" % (os.path.relpath(path, tree), i,
                                                    l.strip()[:120]))
        if hits:
            findings.append({
                "location": hits[0].split(":")[0],
                "title": "Removed function still referenced: %s()" % name,
                "why": "%s() is defined in %s but not in %s" % (
                    name, short(c.old), short(c.new)),
                "body": "\n".join(hits[:15]) + (
                    "\n... %d more" % (len(hits) - 15) if len(hits) > 15 else ""),
                "replay": "git grep -nw %s %s" % (name, short(c.new)),
                "rank": 3,
            })
    return result("stale", findings, ran=len(removed))


# ---- global-state footprint -------------------------------------------

def footprints(c, sha):
    cache = os.path.join(c.work, sha, "footprint.json")
    if os.path.exists(cache) and os.path.getmtime(cache) >= \
            os.path.getmtime(os.path.join(HERE, "footprint.py")):
        return textio.load_json(cache)
    import footprint
    fp = footprint.analyse(os.path.join(c.target(sha), "src"))
    textio.dump_json(fp, cache)
    return fp


def _rw(v):
    return ("r" if v[0] else "") + ("w" if v[1] else "")


def _delta(old, new):
    """{(path, 'r'|'w'): +1 gained / -1 lost} between two totals."""
    d = {}
    for p in set(old) | set(new):
        o, n = old.get(p, [0, 0]), new.get(p, [0, 0])
        for i, kind in ((0, "r"), (1, "w")):
            if n[i] and not o[i]:
                d[(p, kind)] = 1
            elif o[i] and not n[i]:
                d[(p, kind)] = -1
    return d


def _via(fp, key, path, kind):
    """Where a function's access to path comes from: directly, or via
    which callee (first in call order)."""
    import footprint
    f = fp[key]
    i = 0 if kind == "r" else 1
    if f["access"].get(path, [0, 0])[i]:
        return "directly"
    for callee, args, direct in f["calls"]:
        if callee not in fp:
            continue
        for q, v in fp[callee]["total"].items():
            if not v[i]:
                continue
            sub = footprint.substitute(q, args, fp[callee].get("ptypes", ())) if direct else q
            if sub == path:
                return "via %s()" % callee.split("@")[0]
    return "via a callee"


def check_footprint(c):
    old, new = footprints(c, c.old), footprints(c, c.new)
    c.fp_old, c.fp_new = old, new
    changed = sorted(k for k in set(old) & set(new) if old[k]["src"] != new[k]["src"])
    removed = sorted(set(old) - set(new))
    if not changed:
        return result("footprint", ran=0)
    deltas = {k: _delta(old[k]["total"], new[k]["total"]) for k in changed}

    # Callers of each function in the new call graph, transitively.
    callers = {}
    for k, f in new.items():
        for callee, _, _ in f["calls"]:
            callers.setdefault(callee, set()).add(k)

    def up(k):
        seen, todo = set(), [k]
        while todo:
            for p in callers.get(todo.pop(), ()):
                if p not in seen:
                    seen.add(p)
                    todo.append(p)
        return seen

    def down(k):
        seen, todo = set(), [k]
        while todo:
            x = todo.pop()
            for callee, _, _ in new.get(x, {}).get("calls", []):
                if callee in new and callee not in seen:
                    seen.add(callee)
                    todo.append(callee)
        return seen

    findings = []
    for k in changed:
        d = deltas[k]
        # What a changed callee already reports is not repeated here.
        explained = set()
        for callee in down(k) & set(changed):
            if callee != k:
                explained |= {x for x, s in deltas[callee].items() if d.get(x) == s}
        own = {x: s for x, s in d.items() if x not in explained}
        if not own:
            continue
        f = new[k]
        lines = []
        for sign, verb in ((1, "now"), (-1, "no longer")):
            for kind, what in (("w", "writes"), ("r", "reads")):
                paths = sorted(p for (p, kk), s in own.items() if s == sign and kk == kind)
                if not paths:
                    continue
                lines.append("%s %s:" % (verb, what))
                src = new if sign == 1 else old
                for p in sorted(paths, key=lambda p: (p.startswith(("(", "$")), p))[:12]:
                    lines.append("    %-34s %s" % (p, _via(src, k, p, kind)))
                if len(paths) > 12:
                    lines.append("    ... %d more" % (len(paths) - 12))
        if explained:
            lines.append("(plus %d change(s) inherited from changed callees, "
                         "reported there)" % len(explained))
        ups = sorted(x.split("@")[0] for x in up(k) - set(changed))
        if ups:
            lines.append("inherited by %d unchanged caller(s): %s%s" % (
                len(ups), ", ".join(ups[:6]), ", ..." if len(ups) > 6 else ""))
        gained_w = any(s == 1 and kk == "w" for (p, kk), s in own.items())
        lost_w = any(s == -1 and kk == "w" for (p, kk), s in own.items())
        findings.append({
            "location": "%s:%d" % (f["file"], f["start"]),
            "title": "Global-state footprint changed: %s()" % f["name"],
            "why": "%s() changed; its transitive reads and writes of global "
                   "state compared with %s" % (f["name"], short(c.old)),
            "body": "\n".join(lines),
            "replay": "python3 refcheck/footprint.py SRC OUT.json  (key %s)" % k,
            "rank": 2 if (gained_w or lost_w) else 4,
        })
    return result("footprint", findings, ran=len(changed) + len(removed))


# ---- coverage of changed lines ---------------------------------------

def check_coverage(c):
    if not c.hunks:
        return result("coverage", ran=0)
    tree = c.copy_tree(c.new, "covtree")
    # -O0: line tables that match the fuzzer's coverage build.
    suites, _, _ = run_suite(tree, extra_env={"COV": "--coverage -O0"})
    if not suites:
        return result("coverage", incomplete=["coverage build of the test "
                                              "suite produced no results"])
    # Product code only: coverage of the tests themselves means nothing.
    files = sorted({h["file"] for h in c.hunks if not h["file"].startswith("tests/")})
    findings, incomplete = [], []
    for f in files:
        cnt = gcov_counts(tree, f)
        c.test_counts[f] = cnt
        if cnt is None:
            if os.path.exists(os.path.join(tree, f)):
                incomplete.append("no coverage data for %s" % f)
            continue
        uncovered = []
        for h in c.hunks:
            if h["file"] != f or h["deletion"]:
                continue
            for ln in range(h["start"], h["end"] + 1):
                if cnt.get(ln) == 0:
                    uncovered.append(ln)
        if uncovered:
            ranges = to_ranges(uncovered)
            src = textio.read(os.path.join(tree, f)).splitlines()
            body = []
            fz = getattr(c, "fuzz_unrun", None)
            for a_, b_ in ranges[:8]:
                note = ""
                if fz is not None:
                    unrun = set(fz.get(f, []))
                    if all(l in unrun for l in range(a_, b_ + 1)):
                        note = "  (fuzzer never ran it either)"
                    elif not any(l in unrun for l in range(a_, b_ + 1)):
                        note = "  (the fuzzer does run it)"
                body.append("%s:%s%s" % (f, a_ if a_ == b_ else "%d-%d" % (a_, b_), note))
                for ln in range(a_, min(b_, a_ + 3) + 1):
                    body.append("    %4d  %s" % (ln, src[ln - 1].rstrip()[:100]))
            findings.append({
                "location": f,
                "title": "Changed lines no test runs",
                "why": "always (test-suite coverage of changed lines)",
                "body": "\n".join(body),
                "replay": "make test CFLAGS=--coverage LDFLAGS=--coverage && gcov %s" % f,
                "rank": 4,
            })
    return result("coverage", findings, incomplete, ran=len(files))


def gcov_counts(tree, f):
    import glob
    import gzip
    for g in glob.glob(os.path.join(tree, "*.gcov.json.gz")):
        os.unlink(g)
    sh(["gcov", "-j", f], cwd=tree)
    counts = None
    for g in glob.glob(os.path.join(tree, "*.gcov.json.gz")):
        data = json.load(gzip.open(g, "rt", encoding="utf-8", errors=textio.KEEP))
        for fe in data.get("files", []):
            if fe["file"].endswith(f):
                counts = counts or {}
                for ln in fe["lines"]:
                    counts[ln["line_number"]] = max(
                        counts.get(ln["line_number"], 0), ln["count"])
    return counts


def to_ranges(lines):
    out = []
    for ln in sorted(lines):
        if out and ln == out[-1][1] + 1:
            out[-1][1] = ln
        else:
            out.append([ln, ln])
    return [tuple(r) for r in out]


# ---- differential fuzzing --------------------------------------------

def check_difffuzz(c):
    old_t, new_t = c.target(c.old), c.target(c.new)
    hfile = os.path.join(c.work, c.new, "hunks.json")
    textio.dump_json(c.hunks, hfile)
    out = os.path.join(c.work, c.new, "difffuzz.json")
    p = sh([sys.executable, os.path.join(HERE, "difffuzz.py"),
            os.path.join(old_t, "difffuzz"), os.path.join(new_t, "difffuzz"),
            "--seeds", "1:%d" % c.a.fuzz_seeds, "--jobs", str(c.jobs),
            "--budget", str(c.a.fuzz_budget),
            "--work", os.path.join(c.work, c.new, "df"), "--out", out,
            "--hunks", hfile, "--cov-bin", os.path.join(new_t, "cov", "difffuzz"),
            "--cov-obj", os.path.join(new_t, "cov", "obj"),
            "--cov-src", os.path.join(new_t, "src")])
    if p.returncode != 0 or not os.path.exists(out):
        return result("difffuzz", incomplete=["differential fuzzing failed: "
                                              + (p.stderr or p.stdout)[-400:]])
    d = textio.load_json(out)
    findings, incomplete = [], []
    for f in d["findings"]:
        findings.append(report.fuzz_finding(f, c))
    n = d["seeds_run"]
    cnt = d["counts"]
    if cnt.get("incomplete"):
        incomplete.append("%d of %d fuzz seeds did not run on the old build"
                          % (cnt["incomplete"], n))
    if cnt.get("old-nondeterministic"):
        incomplete.append("%d fuzz seeds were nondeterministic on the old build "
                          "and were not compared (fields: %s)"
                          % (cnt["old-nondeterministic"],
                             ", ".join(d["old_nondeterministic_fields"])))
    for u in d.get("unminimised", []):
        incomplete.append("divergence group %s (%d seeds, first seed %d): %s"
                          % (u["signature"], len(u["seeds"]), u["seeds"][0],
                             u.get("reason", "not minimised")))
    if d.get("unreached_hunks"):
        where = ", ".join("%s:%s" % (f, ",".join(
            "%d" % a_ if a_ == b_ else "%d-%d" % (a_, b_)
            for a_, b_ in to_ranges(ls)))
            for f, ls in sorted(d["unreached_hunks"].items()))
        incomplete.append("the fuzzer never ran these changed lines (first %d "
                          "seeds), so its silence says nothing about them: %s"
                          % (d["cov_seeds"], where))
    c.fuzz_unrun = d.get("unreached_hunks", {})
    c.fuzz_counts = {f: {int(k): v for k, v in cnt.items()}
                     for f, cnt in d.get("fuzz_counts", {}).items()}
    if n < c.a.fuzz_seeds:
        incomplete.append("fuzz budget reached after %d of %d seeds"
                          % (n, c.a.fuzz_seeds))
    return result("difffuzz", findings, incomplete, ran=n)


CHECKS = {
    "warnings": check_warnings,
    "tests": check_tests,
    "newtests": check_newtests,
    "stale": check_stale,
    "footprint": check_footprint,
    "coverage": check_coverage,
    "difffuzz": check_difffuzz,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--work", default="refcheck-work")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--fuzz-seeds", type=int, default=2000)
    ap.add_argument("--fuzz-budget", type=float, default=600)
    ap.add_argument("--skip", default="")
    a = ap.parse_args()
    c = Ctx(a)
    skip = {s for s in a.skip.split(",") if s}
    results, t0 = [], time.time()
    for name in ALL_CHECKS:
        if name in skip:
            continue
        t = time.time()
        try:
            r = CHECKS[name](c)
        except BuildFailed as e:
            r = result(name, findings=[{
                "location": "build", "title": "Commit %s does not build" % short(e.sha),
                "why": "always", "body": e.log, "replay": "make", "rank": 0}])
        except Exception as e:  # a broken check must not read as clean
            r = result(name, incomplete=["check %s crashed: %s: %s"
                                         % (name, type(e).__name__, e)])
        r["seconds"] = round(time.time() - t, 1)
        results.append(r)
        print("refcheck: %-9s %3d finding(s) %d incomplete  %5.0fs"
              % (name, len(r["findings"]), len(r["incomplete"]), r["seconds"]),
              file=sys.stderr)
    md = report.render(c, results, time.time() - t0) + context.render(c)
    base = os.path.join(c.work, "report-%s" % short(c.new))
    textio.write(base + ".md", md)
    textio.dump_json({"old": c.old, "new": c.new, "results": results},
                     base + ".json", indent=1)
    print(base + ".md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
