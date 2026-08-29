# Building emil for Genode

[Genode](https://genode.org) is a capability-based component framework
that runs on several kernels, including the formally verified seL4.  It
is not a Unix. A process has no ambient filesystem and no shell; it sees
only the VFS its runtime configuration grants it.

`fork()` does exist here, contrary to a common assumption about the
platform. Genode's libc has implemented it since 2019, in
`src/lib/libc/fork.cc`, by starting a fresh component and cloning the
parent's address space over a clone session; both `fork` and `vfork` are
exported from `libc.lib.so`. It is considerably more expensive than a
Unix fork and carries constraints a Unix program would not expect, but
the constraint is cost, not absence.

The port does not depend on that either way. The editor spawns
subprocesses through `posix_spawn`, which Genode also provides, so it
never takes the fork path at all. What is genuinely missing is anything
to spawn: a Genode component starts with an empty VFS and there is no
`/bin/sh` in it. `EMIL_DISABLE_SHELL` already compiles out the
shell-integration paths for Android, so `emil_subprocess.c` and `pipe.c`
reduce to their stub halves and this port needs no change of its own —
the same exclusion, for the same reason.

## Layout

    tests/genode/stage.sh          assembles emil/src from the repository root
    tests/genode/run_component.sh  boots a prepared run dir, non-interactively
    tests/genode/run_suites.sh     runs every unit suite, one boot each
    tests/genode/rundir.sh         prepares a run dir from another runtime
    tests/genode/smoke-runtime     LOG-only runtime for the version smoke test
    tests/genode/test-runtime      runtime for the unit suites
    tests/genode/test-archives     extra depot archives the suites need
    tests/genode/emil/Makefile.in  the cross build; becomes src/Makefile
    tests/genode/emil/used_apis    genodelabs/api/{libc,posix}
    tests/genode/emil/artifacts    what the build is expected to produce
    tests/genode/emil/version      depot archive version (a date, per goa)
    tests/genode/emil/pkg/         runtime configuration and depot archives

Goa requires `pkg/runtime` at exactly that path. The nested
`pkg/<name>/runtime` form is rejected by `looks_like_goa_project_dir` in
goa's `share/goa/lib/util.tcl`, which reports only that the directory
"does not look like a goa project" and names neither the file nor the
reason. `pkg/README` is likewise required, though not by `goa build` —
only `goa export` asks for it.

Goa requires each project's sources under `<project>/src` and binds that
directory into a bubblewrap sandbox read-only, so `stage.sh` copies
rather than symlinks: a link into the parent would resolve to a path the
sandbox has not bound, and the compiler would report a missing file
instead of a missing bind.

## Building locally

Install [goa](https://codeberg.org/genodelabs/goa), put its `bin` on
`PATH`, then:

    ./tests/genode/stage.sh
    goa build -C genode/emil --arch x86_64

Goa downloads the pinned toolchain and the API archives from
`depot.genode.org` on first run and caches them under `tests/genode/emil/var`.
That directory reaches about 680 MB, nearly all of it the toolchain: a
230 MB tarball plus the squashfs built from it. Both have to be kept —
goa's install rule declares no `.SECONDARY`, so deleting the tarball
only causes it to be downloaded again on the next build.

To produce depot archives as well as a binary:

    goa export -C genode/emil --arch x86_64 --license "$PWD/LICENSE"

The top-level `LICENSE` has to be named explicitly because it lives at
the repository root rather than in the project directory.

Goa runs its downloader inside bubblewrap with `--clearenv`, so build
environment variables do not reach it. That matters mainly when
diagnosing failures: a `tar` or `curl` setting exported in the shell has
no effect on what goa does.

## Two goa bugs this port works around

Both are upstream, in goa itself; neither is caused by anything here.

**The toolchain cannot be installed where `/bin/sh` is dash.**
`share/goa/lib/install_tool.mk` sets `ECHO := echo -e` but, unlike its
sibling `sync_http.mk`, never sets `SHELL := bash`. make runs the rule
under `/bin/sh`, and dash's `echo` has no `-e`, so it prints the flag
literally: the line fed to `sha256sum -c` becomes
`-e <sha>  <file>`, which cannot be parsed. The check fails, the rule
deletes the 230 MB tarball it just downloaded, and the build aborts with
`Unable to install genode-toolchain-25.05`. This is not intermittent —
it fails on every cold install on Debian, Ubuntu, and anything else
where `/bin/sh` is dash. It was confirmed directly: a byte-perfect
tarball with the correct hash fails the check under dash and passes it
under bash.

make deliberately ignores `SHELL` from the environment, so this cannot
be overridden from outside. CI therefore fetches the tarball itself,
verifying it against the same pins read out of `install_tool.mk`, and
leaves it where the rule expects it; make then sees its target up to
date and never runs the broken recipe. Locally, the same effect comes
from having installed the toolchain once by any means. The workaround
can go once goa adds `SHELL := bash`.

**The toolchain mount is checked too early.** After spawning
`squashfuse_ll`, `lib/actions/build.tcl` waits a flat 100 ms and reports
`Installation of tool chain ... failed` if the compiler is not yet
visible. On a cold 480 MB image on a loaded machine that is a race
rather than a real failure; simply running the build again succeeds,
which is why the CI job retries once.

## Expected warnings

The build is free of errors but not of warnings, and neither warning is
a regression:

- **`"CTRL" redefined`**, once per translation unit. Genode's
  `<termios.h>` includes `sys/ttydefaults.h`, which defines `CTRL`
  unconditionally. `keymap.h` guards its own definition with
  `#ifndef CTRL`, but that guard does not help here: `emil.h` includes
  `keymap.h` before `<termios.h>`, so emil's macro is defined first and
  the system's redefinition wins for the rest of the unit.

  This is currently harmless, and that was checked rather than assumed.
  All 30 call sites pass a character literal — `a`–`z`, `W`, `C`, `@`,
  `_` — and emil's `(x) & 0x1f` and the ttydefaults form agree on every
  one of them. They diverge for arguments emil does not yet use:
  `CTRL(' ')` is 0 under emil and 96 under ttydefaults, `CTRL('?')` is
  31 versus 127, and every digit differs. Ctrl-Space is spelled
  `CTRL('@')`, which is safe. A future `CTRL(' ')` would be wrong on
  Genode only, and silently.

- **`ISO C99 does not support '_Generic'`**, once, from Genode's
  `sys/cdefs.h` by way of `libgen.h`'s `dirname`. Goa passes the depot
  include directories with `-I` rather than `-isystem`, so `-Wpedantic`
  applies to system headers too. Nothing in this repository can fix that
  short of dropping `-Wpedantic` for this target.

## What is and is not covered

CI builds, links, checks that the result is an ELF carrying Genode's
`ld.lib.so` interpreter — a host-toolchain binary would also be an ELF,
so the interpreter is what actually distinguishes them — and then runs
the editor: `emil --version` is executed on Genode under `base-linux`
and its output and exit status are checked. `base-linux` runs a whole
Genode system as ordinary Linux processes, so this needs no QEMU, no
hardware and no display.

That path was chosen because `--version` is handled in `main()` before
raw mode and before the event loop, so it is the one editor path that
needs no terminal. It proves the binary loads, that the libc starts,
that argument handling works, and that `-DEMIL_VERSION` reached the
cross build — the last of which is otherwise invisible, since a binary
that reports itself as "unknown" still links and still runs.

`goa run` cannot be used for this. It ends in an expect `interact` with
`timeout -1`, waiting for a human to press Ctrl-C; in CI it returns
immediately having printed nothing. `tests/genode/run_component.sh` therefore
drives Genode's `core` directly, on a directory prepared by
`goa run-dir`. Two properties of `core` shape that script: it never
exits — when its child finishes, init keeps running and core sits there
forever, and since it writes nothing further it never takes SIGPIPE
either — and it has no useful exit status of its own. The child's status
instead arrives as a line of log output from init, which is what the
script parses. The script is written to be reusable for running the unit
suites later.

Note that the component label in the log is the *project directory*
name, not the binary name.

What is still not covered is interactive editing. The pty scenarios
cannot run at all. Genode's libc exports `grantpt`, `unlockpt` and
`ptsname`, but not `posix_openpt`, `openpty` or `forkpty` — the
functions that configure a pty are there while the ones that create one
are not — and `tests/decoder_pty_test.c` builds its terminal by calling
`posix_openpt`. That is the same boundary the Haiku pin ran into,
reached from the other direction.

Note that this is a limit on creating a pty, not on terminal handling:
`tcgetattr`, `tcsetattr` and `ioctl` are all present. Exercising the
editor's raw-mode path on Genode is therefore not ruled out, but it
would have to be driven through Genode's own terminal component rather
than by reusing the existing harness.

## The unit suites

The editor's own suites are cross-built by the same toolchain and run on
Genode, one boot per suite at roughly 120ms each. At the time of writing
that is 26 suites passing, 0 failing, and 5 reported as expected
failures.

This is not a duplicate of the FreeBSD job, even though Genode's libc is
FreeBSD-derived. The source lineage is shared; the runtime is not. The
file-based suites — `fileio`, `insert_file`, `relpath`, `tilde`,
`ctags` — exercise `fileio.c` against Genode's VFS, with no Unix kernel
underneath at all, and `fileio.c` is the largest module in the editor.

Three suites are not built, and each names something that does not exist
here rather than something skipped for convenience. `test_shell` and
`test_subprocess` call the shell-integration API that
`EMIL_DISABLE_SHELL` compiles out, so they do not link. `test_warnings`
drives the host compiler. The list is `UNBUILDABLE` in `Makefile.in`.

Five suites — `test_unicode`, `test_wcwidth`, `test_cjk_indic`,
`test_display`, `test_status_bar` — used to fail here, because they
asserted widths that require a UTF-8 locale. They now assert what emil
does *given what the platform offers*: `selectUtf8Locale()` reports
whether a UTF-8 `LC_CTYPE` was obtained, and the expected widths are
written as `wideCols()` and `combiningCols()` rather than the literals
2 and 0. Both answers are asserted, so this is not an exemption; see
"Unicode width" below. They pass here on their own merits.

`test_backup` needs a generated `tests/backup_faked.c`, produced by
rewriting `backup.c`'s syscalls to the fakes the suite defines.
`stage.sh` has to generate it before goa starts, and `run_tests.sh`
generates it for the native build, so the rewrite — and its assertion on
the number of call sites — lives in `tests/make_backup_fixture.sh` and is
called from both. It used to be written out twice, with the expected
count duplicated.

Each suite is run in its own freshly booted Genode system rather than as
one of many children of a single init. That costs a boot per suite and
buys isolation: one suite's failure, or its RAM exhaustion, cannot
perturb the next, and each gets its own exit status without anyone
having to disentangle an interleaved log.

Three runtime requirements were each found by hitting them, and are
declared in `tests/genode/test-runtime`: a `<timer/>` in `<requires>`, without
which init denies the Timer session and the component dies before
running a test; an RNG, because `mkstemp` needs entropy; and a `/tmp`
backed by `<ram/>`, because the file-based suites create files there
with `mkstemp` and `mkdtemp`. The RNG is `jitterentropy` rather than a
`<zero/>` node named `random`, which appears to work and hands out a
constant.

## Unicode width

Genode ships only the C locale, and this is worth stating precisely
because the symptom looks like a width bug and is not one.

It is a build-time decision, not a missing runtime configuration: the
libc port imports FreeBSD 12.0's libc, which has full locale support,
and then `repos/libports/lib/mk/libc-locale.mk` filters `setlocale.c`,
`setrunelocale.c` and every encoding module including `utf8.c` out of
the build, linking `nolocale.cc` stubs in their place. The comment there
reads "strip locale support down to \"C\"". No VFS mount or runtime
config restores it. This is conforming — POSIX requires exactly one
locale to exist, `"C"` — and it is what the mainline libc does.

`setlocale(LC_CTYPE, ...)` returns `"C"` for every argument tried,
including `"C.UTF-8"` and `"en_US.UTF-8"`; `MB_CUR_MAX` stays 1, and
`mbrtowc` on a UTF-8 sequence consumes a single byte and yields the
Latin-1 character. In that locale `wcwidth` correctly returns -1 for
every non-ASCII codepoint, because in the C locale those characters have
no defined width. Genode is not computing widths wrongly; it has no
UTF-8 locale to compute them in.

The one sharp edge is that `setlocale` reports success — returning
`"C"` rather than `NULL` — for a locale it did not set. Code that trusts
the return value would conclude it had a UTF-8 locale when it had not.
`main()` does not: it confirms with `wcwidth(0x4E00) == 2` and only then
accepts the locale, so it correctly falls through all its candidates
here.

None of this damages a buffer. The editor decodes UTF-8 with its own
`utf8Decode` and never calls the libc multibyte functions, so text is
read, edited and written correctly. Only display width is delegated, and
`charInStringWidth` already maps a negative `wcwidth` to 1. The visible
consequence is that on Genode every non-ASCII character occupies one
column: CJK and other wide characters are drawn in half the space they
need, and combining marks take a column instead of none. That is the
platform's answer being passed through, which is the intended behaviour.

The width-dependent suites used to fail here for exactly that reason:
they hardcoded the widths a UTF-8 locale would give — `test_unicode`
called `setlocale(LC_CTYPE, "C.UTF-8")` and then expected `2` for CJK —
which asserts a property of the host rather than of the editor.

They now assert emil's actual contract: decode UTF-8 itself, delegate
width to the platform, map a negative `wcwidth` to 1. All five call
`selectUtf8Locale()` — the same function `main()` calls, so a test
cannot disagree with what the editor will draw — and express expected
widths as `wideCols()` (2 or 1) and `combiningCols()` (0 or 1) instead
of literals. Where a case only exists under a UTF-8 locale, the
*scenario* is parameterised rather than the answer: a wrap width written
`3 * wideCols()` is "room for three characters" on either platform.

Both branches assert something, so nothing is excused and no suite is
skipped. They pass here on their own merits.

## pipe()

`test_writeall` is the only suite here that calls `pipe()`, and it
failed at `startDrain()` until `<pipe/>` was mounted.

Genode's libc does not route `pipe()` to the kernel. It goes through a
VFS plugin, and `Libc::Vfs_plugin::pipe()` begins:

```cpp
Absolute_path base_path(_config.pipe);
if (base_path == "") {
        error(__func__, ": pipe fs not mounted");
        return Errno(EACCES);
}
```

So without `<libc pipe="/dev/pipe"/>` and a `<dir name="pipe"><pipe/></dir>`
in the VFS, every `pipe()` returns -1 with `EACCES`. It also needs the
`vfs_pipe` depot archive and `vfs_pipe.lib.so` in `<content>`; the
plugin lives in `repos/gems`, not `repos/os`, so it is not part of the
base `vfs` archive. `repos/libports/run/libc_integration.run` is the
reference configuration.

This is the fourth requirement found by hitting it, after `timer`, `rng`
and `/tmp`.

Whether `fork()` then works here is a separate question and is not yet
confirmed. Genode's libc implements it by cloning the component --
`fork.cc` transfers 2.5 MB and 100 caps to the child before handing over
`resources.ram_quota` -- and this is the only place in the tree that
calls it, since the editor spawns through `posix_spawn`. If the suite
still fails after the pipe mount, the quotas in `<runtime ram=".."
caps=".."/>` are the next thing to look at.

`goa export` has been run and succeeds. It resolves the full dependency
closure declared by `pkg/runtime` and `pkg/archives` —
`terminal_session`, `rtc_session`, `file_system_session`, `vfs` and the
rest — and produces `src/`, `bin/` and `pkg/` depot archives. That
validates those files as far as goa's schema checks reach.

It does not validate them against a running system. The editor has been
run on Genode, but only as `emil --version`, under the separate
LOG-only smoke runtime; `pkg/runtime` declares the intended wiring for a
real deployment and has never been exercised. Editing for real needs a
terminal session, and termios fidelity over Genode's terminal is the
least certain part of this port. Expect raw mode, window-size reporting,
and the escape stream to need work before the editor is usable rather
than merely runnable.

The CI job is advisory (`continue-on-error: true`) and should stay that
way. It depends on codeberg.org for goa and the toolchain, on
depot.genode.org for the API archives, and on unprivileged user
namespaces for goa's bubblewrap sandbox; any of the three can fail
without the editor being implicated. goa itself is tracked at `main`
rather than pinned, so upstream can change under it. The toolchain is
fetched with a plain `curl -s`, and a truncated transfer fails the SHA
check and aborts the build — which happened twice while this job was
being written, hence the single retry.
