# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Render refcheck results as a Markdown report.

Only findings and incomplete checks are shown; clean checks are reduced
to one line.  Nothing here is generated prose: every line comes from a
tool's output or from a fixed template.
"""

import os
import re
import subprocess

import textio

FIELD_NAMES = {
    "buf": "buffer text", "cur": "cursor", "mark": "mark",
    "status": "echo-area message", "kill": "kill ring", "reg": "registers",
    "win": "windows", "files": "files on disk", "lead": "invariant",
    "screen": "screen",
}


def _snapshots(text):
    """[(header, {row: (text_line, marks_line)})] from a [screen] dump."""
    snaps, cur, last_row = [], None, None
    for l in text.splitlines():
        if l.startswith("  snapshot "):
            cur = (l.strip(), {})
            snaps.append(cur)
            last_row = None
        elif cur is not None:
            m = re.match(r"^\s+(\d+) \|(.*?)\|((?:  past the edge: .*)?)$", l)
            if m:
                last_row = int(m.group(1))
                cur[1][last_row] = (m.group(2) + "|" + m.group(3) if m.group(3)
                                    else m.group(2), "")
            elif last_row is not None:
                t, _ = cur[1][last_row]
                cur[1][last_row] = (t, l[7:])
    return snaps


def _screen_diff(old, new):
    """The first snapshot of the step that differs, as changed rows."""
    so, sn = _snapshots(old), _snapshots(new)
    out = []
    for i in range(max(len(so), len(sn))):
        o = so[i] if i < len(so) else ("(no snapshot)", {})
        n = sn[i] if i < len(sn) else ("(no snapshot)", {})
        if o == n:
            continue
        total = max(len(so), len(sn))
        where = "while the step was drawn" if i < total - 1 else "after the step"
        out.append("snapshot %d of %d (%s)" % (i + 1, total, where))
        oh = re.sub(r"^snapshot \d+, ", "", o[0])
        nh = re.sub(r"^snapshot \d+, ", "", n[0])
        if oh != nh:
            out.append("old " + oh)
            out.append("new " + nh)
        for r in sorted(set(o[1]) | set(n[1])):
            ot, om = o[1].get(r, ("(no row)", ""))
            nt, nm = n[1].get(r, ("(no row)", ""))
            if (ot, om) == (nt, nm):
                continue
            out.append("row %d" % r)
            out.append("  old |%s|" % ot)
            if om or nm:
                out.append("      %s" % om)
            out.append("  new |%s|" % nt)
            if om or nm:
                out.append("      %s" % nm)
        break
    return out


def subject(repo, sha):
    p = textio.run(["git", "-C", repo, "log", "-1", "--format=%s", sha])
    return p.stdout.strip()


def _section_diff(name, old, new):
    """Changed lines of one dump section, old then new, with the buffer
    header kept for context in the buffer section."""
    if name == "screen":
        return _screen_diff(old, new)
    ol, nl = old.splitlines(), new.splitlines()
    out = []
    if name == "buf":
        def by_buffer(lines):
            groups, cur = [], None
            for l in lines:
                if l.startswith("  buffer "):
                    cur = [l, []]
                    groups.append(cur)
                elif cur:
                    cur[1].append(l)
            return groups
        og, ng = by_buffer(ol), by_buffer(nl)
        for i in range(max(len(og), len(ng))):
            o = og[i] if i < len(og) else ["  (no buffer)", []]
            n = ng[i] if i < len(ng) else ["  (no buffer)", []]
            if o == n:
                continue
            if o[0] == n[0]:
                out.append(o[0].strip())
            else:
                out.append("old " + o[0].strip())
                out.append("new " + n[0].strip())
            rows = max(len(o[1]), len(n[1]))
            for r in range(rows):
                orow = o[1][r] if r < len(o[1]) else "      (none)"
                nrow = n[1][r] if r < len(n[1]) else "      (none)"
                if orow != nrow:
                    out.append("  old " + orow.strip())
                    out.append("  new " + nrow.strip())
        return out
    oset, nset = set(ol), set(nl)
    for l in ol:
        if l not in nset:
            out.append("old " + l.strip())
    for l in nl:
        if l not in oset:
            out.append("new " + l.strip())
    return out


def _start_state(init):
    cur = init.get("cur", "").strip()
    m = re.match(r"buffer (\d+), line (\d+), byte (\d+)", cur)
    lines = init.get("buf", "").splitlines()
    out = []
    if m:
        want = "  buffer %s " % m.group(1)
        grab = False
        for l in lines:
            if l.startswith("  buffer "):
                grab = l.startswith(want)
                if grab:
                    out.append("start  " + l.strip()[len("buffer N "):]
                               .strip() + ", cursor line %s byte %s"
                               % (m.group(2), m.group(3)))
                continue
            if grab and len(out) < 7:
                out.append("       " + l.strip())
    return out


def _path(p):
    """Relative to the current directory when inside it, else absolute."""
    rel = os.path.relpath(p)
    return p if rel.startswith("..") else rel


def fuzz_finding(f, c):
    w = f["witness"]
    skipfile = os.path.join(c.work, c.new, "witness-%d.skip" % f["seed"])
    textio.write(skipfile, f["skip"])
    fields = [x for x in f["signature"].split(":", 1)[1].split(",") if x]
    others = sorted({x for s in f.get("signatures", [])
                     for x in s.split(":", 1)[1].split(",") if x} - set(fields))
    kind = f["signature"].split(":", 1)[0]
    title = {
        "divergence": "Behaviour divergence",
        "new-sanitizer": "New sanitizer error",
        "new-hang": "New hang",
        "new-nondeterministic": "New build is nondeterministic",
        "keys-consumed": "Behaviour divergence (prompt reads keys differently)",
    }.get(kind, "Behaviour divergence (%s)" % kind)
    if fields:
        title += ": " + ", ".join(FIELD_NAMES.get(x, x) for x in fields)
    body = ["screen %s · seed %d · seen in %d seed(s)"
            % (w["screen"], f["seed"], len(f["seeds"]))]
    if others:
        body[0] += "; other seeds also differ in: " + ", ".join(
            FIELD_NAMES.get(x, x) for x in others)
    # The starting buffer helps only if the divergence happened in it,
    # not in a file the witness opened later.
    start = _start_state(f["initial"])
    changed = _section_diff("buf", f["old"].get("buf", ""), f["new"].get("buf", ""))
    diverged = {m.group(1) for l in changed
                for m in [re.match(r"(?:old |new )?buffer \d+ (.*?)( modified| read-only| special)*$", l)]
                if m}
    start_name = start[0][len("start  "):].split(", cursor")[0] if start else None
    if start and start_name in diverged:
        body += start
    body.append("keys   " + " ".join(w["keys"]))
    for name in f["old"]:
        d = _section_diff(name, f["old"][name], f["new"].get(name, ""))
        if d:
            body.append("[%s]" % FIELD_NAMES.get(name, name))
            limit = 24 if name == "screen" else 12
            body += ["  " + x for x in d[:limit]]
            if len(d) > limit:
                body.append("  ... %d more lines" % (len(d) - limit))
    if f.get("detail"):
        body.append(f["detail"])
    loc = ", ".join(f["location"]) if f["location"] else \
        "no changed line executed (divergence comes from elsewhere)"
    body.append("changed code run by this witness: " + loc)
    if not w["confirmed"]:
        body.append("(shrunk witness did not reproduce the exact signature; "
                    "replay the full seed)")
    return {
        "location": f["location"][0] if f["location"] else "behaviour",
        "title": title,
        "replayable": True,
        "why": "always (differential keystroke fuzzing, %s vs %s)"
               % (c.old[:7], c.new[:7]),
        "body": "\n".join(body),
        "replay": "python3 refcheck/replay.py %s--seed %d --keep %s %s %s"
                  % ("" if _path(c.work) == "refcheck-work"
                     else "--work %s " % _path(c.work),
                     f["seed"], ",".join(str(i) for i in w["kept_indices"]),
                     c.old[:7], c.new[:7]),
        "rank": 0,
    }


def _file_of(loc):
    if loc.startswith("tests: "):
        return "tests"
    return loc.split(":")[0]


def render(c, results, seconds):
    findings, incomplete, clean = [], [], []
    for r in results:
        findings += r["findings"]
        incomplete += ["%s: %s" % (r["check"], i) for i in r["incomplete"]]
        if not r["findings"] and not r["incomplete"]:
            clean.append(r["check"] + ("" if r["ran"] else " (nothing to check)"))
    head = "### Refactor check: `%s` \"%s\"\n" % (c.new[:7], subject(c.repo, c.new))
    head += "Compared with `%s` · %d checks · " % (c.old[:7], len(results))
    if not findings and not incomplete:
        return head + "nothing to report · %.0f s\n" % seconds
    head += "**%d finding%s** · %d incomplete\n\n" % (
        len(findings), "" if len(findings) == 1 else "s", len(incomplete))

    groups = {}
    for f in findings:
        groups.setdefault(_file_of(f["location"]), []).append(f)
    order = sorted(groups, key=lambda g: (min(f["rank"] for f in groups[g]), g))
    out, n = [head], 0
    for g in order:
        fs = sorted(groups[g], key=lambda f: f["rank"])
        out.append("#### `%s` (%d finding%s)\n" % (g, len(fs), "" if len(fs) == 1 else "s"))
        for f in fs:
            n += 1
            tag = " *(replayable)*" if f.get("replayable") else ""
            out.append("**%d. %s**%s  \nRan because: %s\n```\n%s\n```\nReplay: `%s`\n"
                       % (n, f["title"], tag, f["why"], f["body"].rstrip(), f["replay"]))
    if incomplete:
        out.append("#### Incomplete\n")
        out += ["- %s" % i for i in incomplete]
        out.append("")
    out.append("Clean: %s · %.0f s" % (", ".join(clean) if clean else "none", seconds))
    return "\n".join(out) + "\n"
