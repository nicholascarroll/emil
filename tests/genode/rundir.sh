#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Prepare a goa run directory from a runtime other than the deployed one.
#
# Usage: rundir.sh <project-dir> <arch> <runtime-file> [archives-file]
#
# A goa project has exactly one pkg, so a scenario that is not the
# deployed one -- the version smoke test, the unit suites -- has to be
# run by copying its runtime over pkg/runtime for the duration of
# `goa run-dir` and then putting the original back.  Restoring by copy
# rather than `git checkout` keeps this independent of whether the tree
# is a committed checkout; the trap puts it back however this exits.
#
# This was written inline, twice, in two CI steps that differed only in
# which files they swapped.
#
# goa is retried once.  After mounting the toolchain squashfs with
# squashfuse_ll it waits a flat 100ms and then declares failure if the
# compiler is not yet visible (build.tcl, "check mount availability").
# On a cold image on a loaded runner that is a race, and it reports as
# "Installation of tool chain ... failed".  A second run finds the image
# already unpacked and everything else incremental, so the retry is
# cheap.

set -eu

project=${1:?usage: rundir.sh <project-dir> <arch> <runtime> [archives]}
arch=${2:?usage: rundir.sh <project-dir> <arch> <runtime> [archives]}
runtime=${3:?usage: rundir.sh <project-dir> <arch> <runtime> [archives]}
archives=${4:-}

pkg="$project/pkg"
saved=$(mktemp -d)
# shellcheck disable=SC2064  # $saved must expand now, not at trap time
trap "cp '$saved/runtime' '$pkg/runtime' 2>/dev/null || true;
      cp '$saved/archives' '$pkg/archives' 2>/dev/null || true;
      rm -rf '$saved'" EXIT

cp "$pkg/runtime" "$saved/runtime"
cp "$runtime" "$pkg/runtime"

if [ -n "$archives" ]; then
	cp "$pkg/archives" "$saved/archives"
	cp "$archives" "$pkg/archives"
fi

goa run-dir -C "$project" --arch "$arch" ||
	goa run-dir -C "$project" --arch "$arch"
