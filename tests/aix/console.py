#!/usr/bin/env python3
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
"""Boot the AIX image under QEMU, run guest.sh from the payload CD, and
exit with guest.sh's status.

Usage: console.py <image-dir> <payload.iso> <console-log>

AIX's only interface here is the serial console, so this logs in as
root (no password, per the image's README) and types commands at it.
Everything the VM prints is copied to stdout and to <console-log>.

The disk is opened with -snapshot: nothing is written back to the
qcow2, so the image unpacked from the cache is the same on every run
and QEMU can simply be killed at the end instead of shut down.
"""

import os
import re
import sys
import time

import pexpect

BOOT_TIMEOUT = int(os.environ.get("AIX_BOOT_TIMEOUT", 30 * 60))
RUN_TIMEOUT = int(os.environ.get("AIX_RUN_TIMEOUT", 150 * 60))
# What to run in the unpacked payload.  Override to poke at the VM, e.g.
# AIX_GUEST_CMD="oslevel -s; sh ./guest.sh".  Keep it short: see run().
GUEST_CMD = os.environ.get("AIX_GUEST_CMD", "sh ./guest.sh")
# PS1 is typed as two adjacent strings so that the echo of the command
# that sets it does not itself match the prompt.
PROMPT = "EMIL-AIX# "


class Tee:
    def __init__(self, *streams):
        self.streams = streams

    def write(self, s):
        for st in self.streams:
            st.write(s)
            st.flush()

    def flush(self):
        pass


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    image, iso, log_path = sys.argv[1:]
    qemu = os.path.join(image, "bin", "linux-x86_64", "qemu-system-ppc64")
    disk = os.path.join(image, "aix72.qcow2")
    smp = os.environ.get("AIX_SMP", str(min(os.cpu_count() or 1, 4)))
    mem = os.environ.get("AIX_MEM", "8G")

    # As the image's run.sh, except: the payload ISO is in the CD-ROM
    # drive, -snapshot, an explicit CPU count, and no host port
    # forwards.  Keep id=hd0: QEMU sends it as the disk's unique ID,
    # and AIX's ODM knows the disk by it.
    args = [
        "-name", "AIX 7.2 POWER PSERIES",
        "-M", "pseries,ic-mode=xics", "-cpu", "POWER8",
        "-m", mem, "-smp", smp,
        "-serial", "mon:stdio", "-nographic", "-vga", "none",
        "-prom-env", "input-device=/vdevice/vty@71000000",
        "-prom-env", "output-device=/vdevice/vty@71000000",
        "-boot", "order=c",
        "-device", "virtio-scsi-pci,id=scsi",
        "-device", "scsi-hd,drive=hd0",
        "-drive", f"file={disk},if=none,id=hd0,format=qcow2",
        "-drive", f"file={iso},if=scsi,media=cdrom,id=cd0,format=raw",
        "-nic", "user,model=spapr-vlan",
        "-snapshot",
    ]
    log = open(log_path, "w", encoding="utf-8")
    t0 = time.time()

    def note(msg):
        print(f"\n[console.py {time.time() - t0:7.0f}s] {msg}", flush=True)

    note(f"starting QEMU: -smp {smp} -m {mem}")
    vm = pexpect.spawn(qemu, args, encoding="utf-8", codec_errors="replace",
                       timeout=BOOT_TIMEOUT, maxread=65536)
    vm.logfile_read = Tee(sys.stdout, log)

    def run(cmd, timeout=300):
        vm.sendline(cmd)
        vm.expect_exact(PROMPT, timeout=timeout)

    status = 1
    try:
        vm.expect(r"login: ")
        note("login prompt")
        vm.sendline("root")
        # No password; but tolerate an empty one being asked for.
        if vm.expect([r"[Pp]assword:", r"# "], timeout=600) == 0:
            vm.sendline("")
            vm.expect(r"# ", timeout=600)
        vm.sendline("PS1='EMIL-''AIX# '; export PS1")
        vm.expect_exact(PROMPT, timeout=120)
        note("logged in")

        # Long input lines can be truncated by the console line discipline, so the
        # commands are kept short.
        run("lsdev -Cc cdrom")
        run("mount -v cdrfs -o ro /dev/cd0 /mnt && ls -l /mnt")
        run("rm -rf /tmp/emil-aix; cd /tmp && tar xf /mnt/payload.tar")
        run("cd /tmp/emil-aix && ls")
        note(f"payload unpacked; running: {GUEST_CMD}")

        vm.sendline(f"{GUEST_CMD}; echo EMIL_AIX_RC=$?")
        # The echoed command reads "=$?"; only the real status is digits.
        vm.expect(r"EMIL_AIX_RC=(\d+)", timeout=RUN_TIMEOUT)
        status = int(vm.match.group(1))
        note(f"guest exited {status}")
    except pexpect.TIMEOUT:
        note("timed out waiting for the VM; see the console above")
        status = 124
    except pexpect.EOF:
        note("QEMU exited unexpectedly")
        status = 125
    finally:
        vm.terminate(force=True)
        log.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
