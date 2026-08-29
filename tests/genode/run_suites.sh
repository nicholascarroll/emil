#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Run the unit suites on Genode and report.  Exits non-zero if any fails.
#
# Usage: run_suites.sh <project-dir> <component-label> [timeout-seconds]
#
# Booting a fresh Genode system per suite costs about 120ms, so the
# suites are run one at a time rather than as children of a single init.
# That keeps one suite's failure -- or its RAM exhaustion -- from
# perturbing the next, and gives each its own verdict without parsing an
# interleaved log.
#
# `goa run-dir` is called once, by the caller: the run directory is just
# files, so each suite is run by copying its binary in and rewriting the
# binary name in the generated config.
#
# A suite's exit status is its verdict, and run_component.sh carries a
# real one: it parses the child's exit value out of init's log and
# returns it, or returns 125 when the system produced none within the
# timeout.  Nothing here needs to read the suite's output to decide
# anything.
#
# Three suites are not built here (UNBUILDABLE in emil/Makefile.in) --
# the code they test is compiled out by EMIL_DISABLE_SHELL, or needs a
# host compiler.  Everything that is built is run, and nothing is
# excused.

set -u

project=${1:?usage: run_suites.sh <project-dir> <label> [timeout]}
label=${2:?usage: run_suites.sh <project-dir> <label> [timeout]}
limit=${3:-60}

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
run_dir="$project/var/run"
bin_dir="$project/var/build"

[ -f "$run_dir/config" ] || {
	echo "run_suites.sh: no config in $run_dir (run 'goa run-dir' first)" >&2
	exit 125
}

suites=$(find "$bin_dir" -name 'test_*' -type f | sort)
[ -n "$suites" ] || {
	echo "run_suites.sh: no suites found under $bin_dir" >&2
	exit 125
}

placeholder=$(sed -n 's/^ *+ binary \(.*\)$/\1/p' "$run_dir/config" | head -1)
[ -n "$placeholder" ] || {
	echo "run_suites.sh: could not find the binary name in the config" >&2
	exit 125
}
cp "$run_dir/config" "$run_dir/config.tmpl"

total=0
failed=0
failed_names=''

for bin in $suites; do
	suite=$(basename "$bin")
	printf '  %-22s ' "$suite"

	# goa run-dir already placed the placeholder binary here, so for
	# that one suite source and destination are the same file.
	[ "$bin" -ef "$run_dir/$suite" ] || cp "$bin" "$run_dir/$suite"
	sed "s/$placeholder/$suite/g" "$run_dir/config.tmpl" > "$run_dir/config"

	output=$("$here/run_component.sh" "$run_dir" "$label" "$limit" 2>&1)
	rc=$?
	total=$((total + 1))
	if [ "$rc" -eq 0 ]; then
		# Display only, but without it a suite that quietly
		# shrinks looks identical to one that passes.
		echo "PASS ($(echo "$output" | awk '/Tests/{print $1; exit}') tests)"
	else
		if [ "$rc" -eq 125 ]; then
			echo "FAIL (the system reported no exit status)"
		else
			echo "FAIL (exit $rc)"
		fi
		echo "$output" | head -40 | sed 's/^/      /'
		failed=$((failed + 1))
		failed_names="$failed_names $suite"
	fi

	[ "$bin" -ef "$run_dir/$suite" ] || rm -f "$run_dir/$suite"
done

rm -f "$run_dir/config.tmpl"

echo ""
echo "  $total suites: $((total - failed)) passed, $failed failed"
[ "$failed" -eq 0 ] || echo "  failed:$failed_names"

# 31 of 34 are built here.  The floor only has to catch a staging
# failure that produced an empty build directory.
if [ "$total" -lt 25 ]; then
	echo "  only $total suites ran; expected at least 25."
	echo "  Nothing was proven; treat this as a staging failure."
	exit 1
fi
[ "$failed" -eq 0 ]
