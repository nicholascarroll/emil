# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
"""Global-state footprints of emil's functions, from clang's AST.

For every function defined in the tree: which global state it reads and
which it may write, directly and through everything it calls.

Global state is emil's file-scope and function-static variables, and
paths into them up to two members deep: `E.statusmsg`, `E.buf->dirty`,
`E.kill.str`.  Deeper accesses are folded into their two-member prefix
(`E.buf->row[i].size = 0` writes `E.buf->row`).

An access is a read, a write, or both; taking an address or letting an
array decay into a pointer that is passed on counts as both, because the
receiver may do either.

Writes through a pointer parameter are kept as `$N->field` and, at each
call, rewritten in terms of the argument: `markDirty(E.buf)` turns the
callee's `$0->dirty` into `E.buf->dirty`, and a caller forwarding its own
parameter keeps it as its own `$M`.  So replacing a global access by a
parameter the caller passes the same global to is not a change.

  python3 refcheck/footprint.py SRCDIR OUT.json
"""

import hashlib
import json
import os
import re
import subprocess
import sys

import textio

MAX_MEMBERS = 2
FLAGS = ["-std=c99", "-D_DEFAULT_SOURCE", "-D_BSD_SOURCE",
         '-DEMIL_VERSION="refcheck"', "-fsyntax-only", "-Xclang",
         "-ast-dump=json", "-Wno-everything"]


def ast(srcdir, f):
    p = textio.run(["clang"] + FLAGS + ["-I", srcdir, f], cwd=srcdir,
                   errors=textio.KEEP)
    if not p.stdout:
        raise RuntimeError("clang failed on %s: %s" % (f, p.stderr[-500:]))
    return json.loads(p.stdout)


class FileTracker:
    """clang writes a location's file only when it differs from the
    previous location printed; replay that to know each node's file."""

    def __init__(self):
        self.cur = None

    def loc(self, l):
        if not isinstance(l, dict):
            return self.cur
        if "spellingLoc" in l:
            self.loc(l["spellingLoc"])
            return self.loc(l.get("expansionLoc"))
        if "file" in l:
            self.cur = l["file"]
        return self.cur

    def node(self, n):
        f = self.loc(n.get("loc"))
        r = n.get("range")
        if isinstance(r, dict):
            self.loc(r.get("begin"))
            self.loc(r.get("end"))
        return f


def struct_of(t):
    """'buffer' for a pointer to struct buffer (or to a typedef of a
    struct), else None."""
    q = t.get("desugaredQualType") or t.get("qualType") or ""
    m = re.match(r"^(?:const )?(?:struct|union) (\w+) \*(?: ?const)?$", q)
    if m:
        return m.group(1)
    m = re.match(r"^(?:const )?(\w+) \*$", t.get("qualType") or "")
    if m and q.startswith(("struct ", "union ", "const struct ")):
        return m.group(1)
    return None


def strip(n):
    while n.get("kind") in ("ParenExpr", "ImplicitCastExpr", "CStyleCastExpr") and n.get("inner"):
        n = n["inner"][0]
    return n


class TU:
    def __init__(self, srcdir, fname):
        self.srcdir, self.fname = srcdir, fname
        self.text = open(os.path.join(srcdir, fname), "rb").read()
        self.tree = ast(srcdir, fname)
        self.globals = {}   # decl id -> name
        self.funcs = {}     # key -> info
        self.static_fns = {}  # decl id / name -> key for static functions
        self.tracker = FileTracker()
        self._scan()

    def key_of(self, name, static):
        return "%s@%s" % (name, self.fname) if static else name

    def _is_emil_file(self, f):
        return f is not None and not f.startswith("/")

    def _scan(self):
        # One pass in document order: file tracking needs every node.
        main = self.fname
        tops = []
        for n in self.tree.get("inner", []):
            f = self.tracker.node(n)
            in_main = "includedFrom" not in (n.get("loc") or {}) and \
                not (n.get("loc") or {}).get("spellingLoc", {}).get("includedFrom")
            tops.append((n, f, in_main))
            self._walk_files(n.get("inner", []))
        for n, f, in_main in tops:
            k = n.get("kind")
            if k == "VarDecl" and (in_main or self._is_emil_file(f)):
                self.globals[n["id"]] = n["name"]
            elif k == "FunctionDecl" and n.get("storageClass") == "static":
                self.static_fns[n["id"]] = self.key_of(n["name"], True)
                self.static_fns[n["name"]] = self.key_of(n["name"], True)
        for n, f, in_main in tops:
            if n.get("kind") == "FunctionDecl" and in_main and \
                    any(c.get("kind") == "CompoundStmt" for c in n.get("inner", [])):
                self._function(n)

    def _walk_files(self, nodes):
        for n in nodes:
            if isinstance(n, dict):
                self.tracker.node(n)
                self._walk_files(n.get("inner", []))

    def _function(self, fn):
        name = fn["name"]
        key = self.key_of(name, fn.get("storageClass") == "static")
        params, ptypes = {}, []
        for c in fn.get("inner", []):
            if c.get("kind") == "ParmVarDecl":
                params[c["id"]] = len(params)
                ptypes.append(struct_of(c.get("type", {})))
        local_types = {}  # local pointer-to-struct variables
        r = fn["range"]
        b, e = r["begin"].get("offset"), r["end"].get("offset")
        src = self.text[b:e + 1] if b is not None and e is not None else b""
        start = self.text.count(b"\n", 0, b) + 1 if b is not None else 0
        info = {"name": name, "file": self.fname, "start": start,
                "end": start + src.count(b"\n"),
                "src": hashlib.sha1(re.sub(rb"\s+", b" ", src)).hexdigest(),
                "access": {}, "calls": [], "ptypes": ptypes}
        local_statics = {}

        def add(path, r, w):
            a = info["access"].setdefault(path, [0, 0])
            a[0] |= r
            a[1] |= w

        def root_path(node, stack):
            """(path, members, top index in stack) for an lvalue chain
            rooted at a global or a pointer parameter, else None."""
            ref = node.get("referencedDecl", {})
            rid = ref.get("id")
            if rid in self.globals:
                path, is_param = self.globals[rid], False
            elif rid in local_statics:
                path, is_param = local_statics[rid], False
            elif rid in params:
                path, is_param = "$%d" % params[rid], True
            elif rid in local_types:
                # Whatever object of this type the local points at.
                path, is_param = "(%s)" % local_types[rid], True
            else:
                return None
            members, i, cur = 0, len(stack) - 1, node
            arrow_seen = False
            while i >= 0:
                p = stack[i]
                pk = p.get("kind")
                first = p.get("inner", [None])[0] is cur
                if pk == "MemberExpr" and first:
                    if members < MAX_MEMBERS:
                        path += ("->" if p.get("isArrow") else ".") + p["name"]
                        members += 1
                    arrow_seen |= bool(p.get("isArrow"))
                elif pk == "ArraySubscriptExpr" and first:
                    arrow_seen = True
                elif pk == "ParenExpr":
                    pass
                elif pk == "ImplicitCastExpr" and i > 0 and \
                        stack[i - 1].get("kind") in ("MemberExpr", "ArraySubscriptExpr") and \
                        stack[i - 1].get("inner", [None])[0] is p and \
                        (stack[i - 1].get("kind") == "ArraySubscriptExpr" or
                         stack[i - 1].get("isArrow")):
                    pass  # pointer or array on the way to a deref
                elif pk == "UnaryOperator" and p.get("opcode") == "*":
                    arrow_seen = True
                else:
                    break
                cur = p
                i -= 1
            if is_param and not arrow_seen:
                return None  # the parameter itself, not what it points to
            return path, cur, i

        def classify(top, i, stack):
            if i < 0:
                return 1, 0
            p = stack[i]
            pk = p.get("kind")
            first = p.get("inner", [None])[0] is top
            if pk == "ImplicitCastExpr":
                ck = p.get("castKind")
                if ck == "LValueToRValue":
                    return 1, 0
                if ck in ("ArrayToPointerDecay", "FunctionToPointerDecay"):
                    return 1, 1
                return 1, 0
            if pk == "UnaryOperator":
                op = p.get("opcode")
                if op == "&":
                    return 1, 1
                if op in ("++", "--"):
                    return 1, 1
                return 1, 0
            if pk == "BinaryOperator" and p.get("opcode") == "=" and first:
                return 0, 1
            if pk == "CompoundAssignOperator" and first:
                return 1, 1
            return 1, 0

        def arg_path(a):
            a = strip(a)
            stack, cur = [], a
            # Walk down to the DeclRefExpr, remembering the chain.
            while cur.get("kind") in ("MemberExpr", "ArraySubscriptExpr", "ParenExpr",
                                      "ImplicitCastExpr", "UnaryOperator") and cur.get("inner"):
                if cur.get("kind") == "UnaryOperator" and cur.get("opcode") not in ("&", "*"):
                    return None
                stack.append(cur)
                cur = cur["inner"][0]
            if cur.get("kind") != "DeclRefExpr":
                return None
            rp = root_path(cur, stack)
            if rp is None:
                ref = cur.get("referencedDecl", {})
                if ref.get("id") in params:  # forwarding a parameter
                    if not stack or all(s.get("kind") in ("ImplicitCastExpr", "ParenExpr")
                                        for s in stack):
                        return "$%d" % params[ref["id"]]
                return None
            path, top, i = rp
            # `&E.kill` passes the object E.kill: its fields are reached
            # through the callee's $N->field.
            if i >= 0 and stack[i].get("kind") == "UnaryOperator" and stack[i].get("opcode") == "&":
                return path + "&"
            return path

        def walk(n, stack):
            k = n.get("kind")
            if k == "VarDecl" and n.get("storageClass") == "static":
                local_statics[n["id"]] = "%s::%s" % (key, n["name"])
            elif k == "VarDecl":
                t = struct_of(n.get("type", {}))
                if t:
                    local_types[n["id"]] = t
            if k == "DeclRefExpr":
                ref = n.get("referencedDecl", {})
                if ref.get("kind") == "FunctionDecl":
                    callee = self.static_fns.get(ref.get("id")) or \
                        self.static_fns.get(ref.get("name")) or ref.get("name")
                    call = stack[-2] if len(stack) >= 2 and stack[-2].get("kind") == "CallExpr" \
                        and strip(stack[-2]["inner"][0]) is n else None
                    if call is None and len(stack) >= 1 and stack[-1].get("kind") == "CallExpr":
                        call = stack[-1] if strip(stack[-1]["inner"][0]) is n else None
                    args = [arg_path(a) for a in call["inner"][1:]] if call else []
                    info["calls"].append([callee, args, call is not None])
                else:
                    rp = root_path(n, stack)
                    if rp:
                        path, top, i = rp
                        r, w = classify(top, i, stack)
                        add(path, r, w)
            stack.append(n)
            for c in n.get("inner", []) or []:
                if isinstance(c, dict):
                    walk(c, stack)
            stack.pop()

        for c in fn.get("inner", []):
            if c.get("kind") == "CompoundStmt":
                walk(c, [fn])
        self.funcs[key] = info


def substitute(path, args, ptypes=()):
    """Rewrite a callee's $N-rooted path in terms of the call's args.
    An argument that is not global state becomes its type: "(buffer)"
    for whatever buffer the caller passed."""
    m = re.match(r"^\$(\d+)(.*)$", path)
    if not m:
        return path
    idx, rest = int(m.group(1)), m.group(2)
    if idx >= len(args) or args[idx] is None:
        t = ptypes[idx] if idx < len(ptypes) else None
        return "(%s)%s" % (t, rest) if t else None
    base = args[idx]
    if base.endswith("&"):
        # &E.kill passed: $0->str is E.kill.str
        base = base[:-1]
        rest = re.sub(r"^->", ".", rest)
    full = base + rest
    # keep at most MAX_MEMBERS members past the root
    parts = re.split(r"(->|\.)", full)
    root, tail = parts[0], parts[1:]
    return root + "".join(tail[:2 * MAX_MEMBERS])


def closure(funcs):
    """Transitive {key: {path: [r, w]}} over the call graph."""
    total = {k: {p: list(v) for p, v in f["access"].items()} for k, f in funcs.items()}
    changed = True
    while changed:
        changed = False
        for k, f in funcs.items():
            mine = total[k]
            for callee, args, direct in f["calls"]:
                if callee not in total:
                    continue
                for p, (r, w) in total[callee].items():
                    q = substitute(p, args, funcs[callee].get("ptypes", ())) if direct \
                        else (None if p.startswith("$") else p)
                    if q is None:
                        continue
                    a = mine.setdefault(q, [0, 0])
                    if (a[0] | r, a[1] | w) != tuple(a):
                        a[0] |= r
                        a[1] |= w
                        changed = True
    return total


def analyse(srcdir):
    funcs = {}
    files = sorted(f for f in os.listdir(srcdir) if f.endswith(".c"))
    for f in files:
        funcs.update(TU(srcdir, f).funcs)
    total = closure(funcs)
    for k, f in funcs.items():
        f["total"] = {p: v for p, v in total[k].items() if not p.startswith("$") or True}
    return funcs


def main():
    srcdir, out = sys.argv[1], sys.argv[2]
    funcs = analyse(srcdir)
    textio.dump_json(funcs, out, indent=0)
    print("footprint: %d functions" % len(funcs))


if __name__ == "__main__":
    main()
