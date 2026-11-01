# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Replay a difffuzz witness on the old and new builds.

  python3 refcheck/replay.py --seed N --keep I,J,K OLD NEW
                             [--work DIR] [--gdb old|new]

The witness is the seed plus the indices of the keys it keeps (the
report prints both).  Builds OLD and NEW first if --work does not have
them yet.  Prints the keys, then the readable state of both builds
after the last kept key.  With --gdb, runs that build's driver under
gdb in the same sandbox, stopped at main.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import difffuzz as df  # noqa: E402
import textio  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--work", default="refcheck-work")
    ap.add_argument("--repo", default=".")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--skip", help="skip file (alternative to --keep)")
    ap.add_argument("--keep", help="comma-separated key indices to keep")
    ap.add_argument("--at", type=int, default=None)
    ap.add_argument("--gdb", choices=["old", "new"])
    a = ap.parse_args()
    shas = {k: textio.run(["git", "-C", a.repo, "rev-parse", v],
                              check=True).stdout.strip()
            for k, v in (("old", a.old), ("new", a.new))}
    work = os.path.abspath(a.work)
    bins = {}
    for k, sha in shas.items():
        out = os.path.join(work, sha)
        if not os.path.exists(os.path.join(out, "difffuzz")):
            print("building %s ..." % sha[:7], file=sys.stderr)
            subprocess.run([os.path.join(HERE, "build.sh"), a.repo, sha, out],
                           check=True, stdout=subprocess.DEVNULL)
        bins[k] = os.path.join(out, "difffuzz")
    sb = os.path.join(work, "sb-replay")
    skip = os.path.abspath(a.skip) if a.skip else None
    if a.keep:
        keep = {int(x) for x in a.keep.split(",") if x.strip()}
        _, names = df.nkeys_of(bins["new"], a.seed, sb)
        skip = os.path.join(work, "replay-%d.skip" % a.seed)
        textio.write(skip, "".join("%d\n" % i for i in range(len(names))
                                   if i not in keep))
        if a.at is None:
            a.at = max(keep) + 1
    if a.gdb:
        subprocess.run([df.SANDBOX, sb], check=True)
        os.chdir(sb)
        os.environ.update(df._env(sb))
        os.execvp("gdb", ["gdb", "-ex", "break main", "--args", bins[a.gdb],
                          "trace", str(a.seed), skip or "-"])
    o = df.Run(bins["old"], a.seed, skip, sb)
    n = df.Run(bins["new"], a.seed, skip, sb)
    d = df.first_divergence(o, n)
    at = a.at if a.at is not None else (d[2] if d else 10**9)
    screen, names = df.nkeys_of(bins["new"], a.seed, sb)
    skipped = set()
    if skip:
        skipped = {int(x) for x in textio.read(skip).split()}
    keys = [names[i] for i in range(len(names)) if i not in skipped and i < at]
    print("screen %s, seed %d, %d keys:" % (screen, a.seed, len(keys)))
    print("  " + " ".join(keys))
    print("first divergence: %s" % (df.signature(d) + " at key index %d" % d[2]
                                    if d else "none"))
    for k in ("old", "new"):
        print("\n===== %s (%s) =====" % (k, shas[k][:7]))
        sec = df.dump(bins[k], a.seed, skip, at, os.path.abspath(a.work))
        for name, text in sec.items():
            print("[%s]\n%s" % (name, text))


if __name__ == "__main__":
    sys.exit(main())
