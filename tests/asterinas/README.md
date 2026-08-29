# Asterinas

[Asterinas](https://github.com/asterinas/asterinas) is a Linux
ABI-compatible OS kernel written in Rust. This target builds `emil` and
its unit suites as ordinary x86-64 Linux binaries and runs them on the
Asterinas kernel under QEMU.

## Why this target earns its place

Every other target here answers "does the editor build against a
different libc?" This one answers a different question: **does the
editor's *existing* Linux binary still work when the kernel underneath
it is not Linux?**

Nothing is recompiled for Asterinas. There is no cross toolchain, no
`EMIL_DISABLE_SHELL`, no libc port. Asterinas implements the Linux
syscall ABI directly, so the binary that runs here is the same shape as
the one the `ubuntu-latest` job builds. What is being exercised is the
kernel: its VFS, its process model, its signal delivery, its pty layer
and line discipline — all written in safe Rust against a small unsafe
core (`OSTD`).

That makes this the mirror image of the Genode job. Genode tests the
editor against an unfamiliar libc on a capability-based component
system, and can only build and link the pty paths. Asterinas tests the
editor against a familiar libc on an unfamiliar kernel, and can
actually run them.

## Not gating

`continue-on-error: true`, for now, for three reasons:

1. **Asterinas is pre-production by its own account.** x86-64 is their
   Tier 1 architecture with full CI, and they support 230+ Linux
   syscalls, but their stated 2026 goal is still *advancing toward*
   production readiness. A syscall gap is a plausible cause of a red
   run here, and it is not emil's to fix.

2. **The injection is layout-dependent.** See `inject.sh` — the
   initramfs is Nix-built, the image is a read-only store symlink, and
   the target that produces it is `.PHONY`, so the run step depends on
   `make --old-file=initramfs`. An upstream reorganisation breaks this
   without implicating the editor.

3. **Upstream is pinned but the image is not ours.** The dev container
   (`asterinas/kernel-dev`) is pulled from Docker Hub and carries Nix,
   OSDK, QEMU and the vDSO the build requires.

Promote to gating once it has been green across a few upstream bumps.

## Scope

The unit suites run for real, in one boot, on the Asterinas kernel.
The pty scenarios (`decoder_pty_test`) are built and run but are
**advisory within the advisory job** — their result is printed after
the verdict and never affects it.

That distinction is worth keeping. Asterinas has a genuine pty
subsystem — `kernel/core/src/device/pty` and `device/tty`, with
`termio.rs` and `line_discipline.rs` — and its own regression suite
includes a C test that does `openpty()`, then `tcgetattr`/`tcsetattr`
clearing `ICANON`/`ECHO` and setting `VMIN`/`VTIME`, then reads and
writes across master and slave. It also tracks conformance against
gVisor's `pty_test`. So the syscalls emil needs are implemented and
tested upstream. But driving the *editor* through a pty is a longer
reach than exercising the syscalls directly, and Haiku already
demonstrated that a working pty layer and a working
`posix_openpt`/`fork`/`setsid` harness are not the same claim.

## Static linking, and the one thing to watch

Everything is linked `-static`. The initramfs is Nix-built, so its only
ELF interpreters live at `/nix/store` paths that the build container
does not share; a dynamically linked binary would link cleanly and fail
at `exec` inside the VM.

The known cost: glibc's NSS is `dlopen`-based, so a static binary that
calls `getpwnam`/`getpwuid` resolves nothing at runtime. The editor
reaches those only for `~` expansion, and `test_tilde` is the suite
that would notice. **If `test_tilde` fails on the first green run, that
is glibc-static, not Asterinas** — the fix is to build against musl,
not to weaken the test.

## Expected failures

The five locale-dependent suites (`test_unicode`, `test_wcwidth`,
`test_cjk_indic`, `test_display`, `test_status_bar`) are reported as
expected failures, on the same grounds as Genode: a minimal initramfs
ships only the C locale, so `setlocale(LC_CTYPE, "C.UTF-8")` does not
take and `wcwidth` does not report 2 for CJK.

**Confirmed on the first run.** The list was inherited from Genode on
the guess that a Nix initramfs would be as C-locale-only as a Genode
system, and it turned out to be exactly right: the same five failed,
with the same failure counts Genode reports (9, 2, 3, 4, 2). Nothing to
trim.

`test_warnings` is exempted separately, and only when its *sole*
failure is `test_no_duplicate_buffer_for_symlink_to_open_file` — the
standing #128 reminder that fails on every target including native
Linux. Without that exemption this job would be permanently red for a
reason that has nothing to do with Asterinas. The match is narrow on
purpose: Redox finds a second, real failure in the same suite (`fcntl`
locks across `fork`), and a failure like that appearing here still
turns the suite red.

## What the first run found

28 suites pass, the five locale suites fail as expected, and
`test_warnings` fails only on #128. So every unit suite that asserts
something about the editor passes on a Rust kernel.

The interesting result is in the pty scenarios, and it is a real
difference rather than a gap:

    terminal owned after C-z        FAIL   ECHO/ISIG/ICANON left on after resume
    terminal owned after C-x z      FAIL   (same)
    terminal owned after C-x C-z    FAIL   (same)
    Ctrl-C after C-z ...            SKIP   editor suspended; Ctrl-C not delivered

`spawnEmil()` puts the editor in an orphaned process group. POSIX says
a stop signal sent to a member of one is discarded, and Linux and
illumos do exactly that — so on every other target these scenarios take
the `CHILD_RUNNING` branch and the resume path is never reached.
**Asterinas stops the process instead**, which is the behaviour
`decoder_pty_test.c` already attributes to Cygwin/MSYS2. That takes the
`CHILD_STOPPED` branch, sends `SIGCONT`, and then finds the editor has
not reclaimed the terminal: `ECHO`, `ISIG` and `ICANON` are all still
set after resume.

Two candidate readings, not yet separated:

  * Asterinas diverges from Linux on stop signals to orphaned process
    groups. That part is established by the run — the process stopped.
  * The resume path itself. MSYS2 reaches the same branch and its job
    runs the full pty suite, so if MSYS2 is green then emil's
    `SIGCONT` handling is fine and the second failure is Asterinas's
    `tcsetattr`-after-resume. If MSYS2 turns out not to reach it
    either, this is an emil path that no target has ever exercised.

Worth resolving before promoting the pty scenarios out of advisory.
The cheap experiment is to check whether the MSYS2 job's pty section
reports these three as PASS or SKIP.

## Local reproduction

```sh
git clone https://github.com/asterinas/asterinas /tmp/asterinas
docker run -it --privileged --network=host -v /dev:/dev \
    -v /tmp/asterinas:/root/asterinas -v "$PWD":/root/emil \
    asterinas/kernel-dev:0.18.1-20260805

# inside the container
cd /root/emil   && ./tests/asterinas/build_payload.sh /tmp/payload
cd /root/asterinas && make initramfs AUTO_TEST=boot ENABLE_KVM=0
cd /root/emil   && ./tests/asterinas/inject.sh /root/asterinas /tmp/payload
cd /root/asterinas && make --old-file=initramfs run_kernel AUTO_TEST=boot ENABLE_KVM=0
grep '^EMIL_RESULT:' qemu.log
```

`ENABLE_KVM=0` if the host cannot pass through `/dev/kvm`; it is
slower but works anywhere.
