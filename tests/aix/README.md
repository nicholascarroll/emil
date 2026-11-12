# AIX

AIX 7.2 TL5 SP3 on QEMU pSeries (POWER8, big-endian, XCOFF), fully
emulated. GCC 13.3 from the IBM AIX Toolbox; system `make`; ksh88
`/bin/sh`.

The build and suites use `-Werror`. Every suite and both pty tests must
pass. Any `SKIP` fails the run.

## Scripts

| Script | Runs on | Does |
| --- | --- | --- |
| `fetch.sh` | host | Downloads the image, RPMs and IBM key; checks sha256 pins |
| `stage.sh` | host | Packs source, RPMs, key and `guest.sh` into an ISO |
| `console.py` | host | Boots the VM, logs in on the serial console, runs `guest.sh` |
| `guest.sh` | AIX | Checks RPM signatures, installs GCC, `make`, `make test` |

`packages.txt` lists the RPMs.

## Running locally

Requires x86-64 Linux, `lzip`, `genisoimage` and `python3-pexpect`.

```sh
./tests/aix/fetch.sh ~/aix-cache
lzip -dc ~/aix-cache/aix72pseries-qemu.tar.lz | tar -C ~/aix-cache -xf -
./tests/aix/stage.sh ~/aix-cache /tmp/aix.iso
python3 tests/aix/console.py ~/aix-cache/aix72pseries-qemu /tmp/aix.iso /tmp/aix.log
```

| Variable | Default | Meaning |
| --- | --- | --- |
| `AIX_MEM` | `8G` | VM memory |
| `AIX_SMP` | host CPUs, max 4 | VM CPUs |
| `AIX_GUEST_CMD` | `sh ./guest.sh` | Command run in the guest |

## Mirror

GitHub runners cannot always reach `public.dhe.ibm.com`. The RPMs, their
sources and the key are mirrored as release `v1` of
[emil-aix-deps](https://github.com/nicholascarroll/emil-aix-deps).
The repository variable `AIX_MIRROR` points `fetch.sh` there; IBM is the
fallback. Pins and signatures are checked either way.

To change a pinned RPM, publish a new release with the new `.rpm` and
`.src.rpm` files, then:

```sh
gh variable set AIX_MIRROR --repo nicholascarroll/emil \
    --body https://github.com/nicholascarroll/emil-aix-deps/releases/download/v2
```

## Portability notes

- AIX `<sys/ttychars.h>` defines `CTRL`. emil's macro is `KEY_CTRL`.
- `pty_harness.h` defines `_ALL_SOURCE` on AIX for `struct winsize`.
- AIX has no `mktemp` or `seq`.
- ksh88 parses an unquoted `<` in `${1:?...}` as a redirection.
- AIX `nm` prints the name first: `E U -`.
