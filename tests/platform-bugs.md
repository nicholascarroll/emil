# Suspected platform bugs

Two findings from the cross-target CI, both believed to be defects in the
platform rather than in emil, and both written up here so they can be
reported upstream without re-deriving the evidence.

Neither is worked around in the editor. In each case emil does the right
thing given what the platform provides; what changed on our side was that
the tests stopped asserting a property of the host and started asserting
emil's behaviour given that host.

---

## 1. wasmer: `utimensat()` returns success and does nothing

**Component:** wasmer 7.2.1 (WASIX), filesystem layer
**Severity:** silent data-integrity-adjacent — succeeds while ignoring the request
**Status:** not reported yet

### Summary

`utimensat()` on a WASIX module under wasmer returns `0` and leaves the
file's mtime unchanged. There is no error, no `errno`, and no partial
effect. Callers that check the return value — which is all a correct
caller can do — conclude the timestamp was set when it was not.

### Reproduction

Build any WASIX module that calls `utimensat` with an explicit
`struct timespec[2]` and compare `st_mtime` before and after:

```c
struct stat a, b;
stat(path, &a);
struct timespec t[2];
t[0].tv_sec = a.st_atime; t[0].tv_nsec = 0;
t[1].tv_sec = a.st_mtime + 10; t[1].tv_nsec = 0;
int rc = utimensat(AT_FDCWD, path, t, 0);
stat(path, &b);
printf("rc=%d errno=%d mtime %ld -> %ld (wanted %ld)\n",
       rc, rc ? errno : 0, (long)a.st_mtime, (long)b.st_mtime,
       (long)a.st_mtime + 10);
```

Observed, with the file on a `--volume`-mounted host directory:

```
utimensat rc=0 errno=0  mtime 1788385595 -> 1788385595  (wanted 1788385605)
```

Toolchain: wasi-sdk 33.0 (clang 22), wasix-libc `v2026-07-30.1`,
wasmer 7.2.1, `--target=wasm32-wasmer-wasi`, run as
`wasmer run --volume $PWD:$PWD --cwd $PWD ./module`.

### What does work

A real write moves the mtime correctly, so this is specific to the
explicit-timestamp path rather than to timestamp tracking in general:

```
REWRITE: mtime 1788385614 -> 1788385616  moved=1 | size 4 -> 8
```

### Why the failure mode matters

Returning `0` is worse than returning `-1`/`ENOSYS`. A caller that
handles the unsupported case correctly still gets it wrong here, because
there is nothing to detect. Backup and archive tools that preserve
timestamps will report success having silently dropped them. If the call
cannot be honoured, failing loudly would let callers adapt.

### Effect on emil

None on the editor. Eight tests in `tests/test_warnings.c` used
`utimensat` as a shortcut to simulate another process touching a file,
and all eight failed on WASIX for that reason alone. They now modify the
file for real, which is both portable and closer to what they mean.

---

## 2. Redox / relibc: closing a descriptor does not drop the process's
   record locks on that file

**Component:** relibc / Redox kernel, `fcntl` advisory record locking
**Severity:** POSIX non-conformance; observable difference in lock lifetime
**Status:** not reported yet

### Summary

POSIX.1-2017, `fcntl`, on `F_SETLK` and friends:

> All locks associated with a file for a given process shall be removed
> when a file descriptor for that file is closed by that process or the
> process holding that file descriptor terminates.

That is: closing **any** descriptor referring to an inode drops **every**
record lock the process holds on that inode, not just the locks taken
through the descriptor being closed. It is one of the more surprising
corners of POSIX, and it is load-bearing for any program that holds a
long-lived lock while other code opens and closes the same file.

On Redox this does not appear to hold. A lock taken on one descriptor
survives the close of a second, unrelated descriptor to the same file.

### Reproduction

```c
/* 1. take a write lock on fd A */
int a = open(path, O_RDWR);
struct flock fl = {0};
fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
fcntl(a, F_SETLK, &fl);              /* succeeds */

/* 2. open and immediately close a second descriptor to the same file */
int b = open(path, O_RDONLY);
close(b);

/* 3. ask a *different process* whether a lock is still held */
/*    POSIX: no.  Redox: yes. */
```

Step 3 has to be done from another process, because `F_GETLK` never
reports a caller's own locks as conflicting. `tests/test_warnings.c`
does this by forking a child that runs `F_GETLK` and reports through its
exit status; see `lock_held_on()`.

The assertion that fails on Redox and passes on Linux, macOS, the BSDs,
illumos and Haiku is at `tests/test_warnings.c` in
`test_relock_reports_conflict_when_rival_takes_the_lock`:

```c
TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));   /* baseline: we hold it */
int scratch = open(path, O_RDONLY);
close(scratch);
TEST_ASSERT_EQUAL_INT(0, lock_held_on(path));   /* POSIX: the lock is gone */
```

Four assertions in that test report wrong values under `redoxer`.

### Which direction is safer

Redox is the *more* intuitive behaviour, and arguably the better design.
But it is not what POSIX specifies, and the difference is observable, so
software that relies on the documented rule will behave differently on
Redox than everywhere else.

### Effect on emil

Real, and the reason the rule matters to us. `insertFileAtPath()` opens
and closes the file the buffer is already visiting; on a conforming
system that `fclose` silently drops the lock `markBufferDirty()` took,
while emil's `lock_fd` stays open and emil goes on believing it holds
one. `relockIfDirty()` exists solely to re-assert the lock after any
operation that opens and closes a file.

So on Redox, emil re-asserts a lock it never lost. That is harmless in
itself, but the conflict-detection path that `relockIfDirty()` is
supposed to exercise — noticing that a rival took the lock in the window
— cannot be reached, and that logic is therefore untested on Redox.

We have not changed the editor. `relockIfDirty()` is correct on a
conforming system and harmless on Redox, and adding a platform branch
would trade a real invariant for a workaround.

### Separate, possibly related

`test_subprocess_signal_kills_pipeline` also fails on Redox:
`subprocess_signal()` reaches the immediate child (`sh`) but not the
processes in its group, so a `sleep 30 | { echo ready; cat; }` pipeline
leaves `cat` alive and holding the pipe open. `setpgid()` reports
success, so the process group is created; the signal does not reach its
members. Worth reporting alongside, but it is a different subsystem and
we have not confirmed whether the two share a cause.

---

## Not bugs

Recorded here so they are not re-investigated as such.

**Genode ships only the C locale.** Deliberate and conforming. Its libc
port imports FreeBSD 12.0's libc, which has full locale support, and
then `repos/libports/lib/mk/libc-locale.mk` filters `setlocale.c`,
`setrunelocale.c` and every encoding module including `utf8.c` out of
the build, linking `nolocale.cc` stubs instead. The comment reads "strip
locale support down to `"C"`". POSIX requires exactly one locale to
exist, `"C"`, and in that locale `wcwidth()` returning -1 for non-ASCII
is the correct answer. The one sharp edge is that its `setlocale` stub
returns `"C"` rather than `NULL` for a locale it did not set, so code
trusting the return value concludes wrongly; emil confirms with
`wcwidth(0x4E00) == 2` instead.

**WASIX has no POSIX record locking.** wasi-libc declares `struct flock`
but none of the locking constants, because there is no host lock manager
behind them. Detected as `EMIL_NO_FILE_LOCKING` in `fileio.h` and
compiled out; emil opens, edits and saves normally, and loses only the
warning that a rival holds the file.

**wasmer never cuts `write()` short.** Measured at 12 runs of 12 with a
signal storm arriving during a blocked 512 KB pipe write. Not a defect —
a blocking write completing in full is permitted — but it means
`writeAll()`'s retry path is unreachable there, so `tests/test_writeall.c`
probes for the capability before requiring it.

**Redox has no `diff`.** The redoxer image does not carry it. Emil's
`diffBufferWithFile()` already reports "Diff failed: cannot create
subprocess" and carries on.
