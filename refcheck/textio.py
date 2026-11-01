# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Text in and out of refcheck: always UTF-8, never fatal.

Everything refcheck reads from a tool or a file, and everything it
writes, goes through here, so that:

  - the encoding is UTF-8 whatever the machine's locale, and the same
    commit gives the same report on every machine;
  - a byte that is not valid UTF-8 never raises.  emil is an editor that
    handles raw bytes on purpose, and its failing tests, its compiler
    diagnostics and its witnesses can contain them; a strict decode would
    lose a whole check at exactly the moment it has something to say.

Two policies:

  SHOW (backslashreplace) for text a person reads: a stray byte appears
       as \\xff, which is what the reader needs to see.
  KEEP (surrogateescape) for text refcheck uses as data, such as paths
       from git and clang's AST: odd bytes round-trip unchanged back to
       the filesystem.
"""

import json
import os
import subprocess

ENCODING = "utf-8"
SHOW = "backslashreplace"
KEEP = "surrogateescape"


def decode(b, errors=SHOW):
    return b.decode(ENCODING, errors) if b is not None else ""


# Tools run under one fixed locale.  Their messages depend on it (gcc
# quotes names as ‘x’ in a UTF-8 locale and 'x' in the C locale), and
# refcheck compares tool output between commits and machines.
# LANGUAGE is dropped because gettext lets it override LC_ALL.
LOCALE = "C.UTF-8"


def tool_env(env=None):
    env = dict(os.environ if env is None else env)
    env["LC_ALL"] = LOCALE
    env.pop("LANGUAGE", None)
    return env


def run(cmd, errors=SHOW, **kw):
    """subprocess.run with output captured as bytes and decoded here,
    under the fixed locale."""
    kw["env"] = tool_env(kw.get("env"))
    kw.pop("text", None)
    kw.pop("encoding", None)
    capture = kw.pop("capture_output", True)
    if capture:
        kw.setdefault("stdout", subprocess.PIPE)
        kw.setdefault("stderr", subprocess.PIPE)
    p = subprocess.run(cmd, **kw)
    if isinstance(p.stdout, bytes):
        p.stdout = decode(p.stdout, errors)
    if isinstance(p.stderr, bytes):
        p.stderr = decode(p.stderr, errors)
    return p


def read(path, errors=SHOW):
    with open(path, "rb") as f:
        return decode(f.read(), errors)


def write(path, text):
    with open(path, "w", encoding=ENCODING, errors=SHOW) as f:
        f.write(text)


def load_json(path):
    with open(path, "rb") as f:
        return json.loads(decode(f.read(), KEEP))


def dump_json(obj, path, **kw):
    # ensure_ascii (the default) keeps the file ASCII; surrogate-escaped
    # bytes survive as \\udcXX and come back the same from load_json.
    with open(path, "w", encoding="ascii") as f:
        json.dump(obj, f, **kw)
