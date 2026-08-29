#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Boot a run directory prepared by `goa run-dir`, print the component's
# output, and exit with the component's exit status.
#
# Usage: run_component.sh <run-dir> <component-label> [timeout-seconds]
#
# `goa run` cannot be used for this.  It ends in an expect `interact`
# with `timeout -1`, which waits for a human to press Ctrl-C; under a
# CI shell it returns immediately having printed nothing.  So we drive
# Genode's core ourselves.
#
# Two things about core shape this script.  It never exits: when its
# child is done, init keeps running and core sits there forever, so a
# plain `core` call hangs and even `core | grep -q` does not help --
# core writes nothing further, so it never receives SIGPIPE.  We
# therefore poll the log and kill it.  And core has no exit status of
# its own worth reading; the child's status arrives as a line of log
# output from init, which is what we parse.
#
# The component label is the *project directory* name, not the binary
# name.  A project in genode/emil that builds and runs `emil` appears
# in the log as "[init -> emil]", but a project directory named
# something else would not.

set -u

run_dir=${1:?usage: run_component.sh <run-dir> <label> [timeout]}
label=${2:?usage: run_component.sh <run-dir> <label> [timeout]}
limit=${3:-60}

[ -x "$run_dir/core" ] || {
	echo "run_component.sh: no core in $run_dir (run 'goa run-dir' first)" >&2
	exit 125
}

log=$(mktemp) || exit 125
( cd "$run_dir" && exec ./core ) >"$log" 2>&1 &
core=$!

# Poll rather than block: we need to stop as soon as init reports the
# child's exit value, because nothing after that will ever arrive.
i=0
deadline=$((limit * 10))
while [ "$i" -lt "$deadline" ]; do
	if grep -q "child \"$label\" exited with exit value" "$log" 2>/dev/null; then
		break
	fi
	kill -0 "$core" 2>/dev/null || break
	i=$((i + 1))
	sleep 0.1
done

kill "$core" 2>/dev/null
wait "$core" 2>/dev/null

# The component's own output, with init's per-component prefix removed,
# so callers see exactly what the program printed.
sed -n "s/^\[init -> $label\] //p" "$log"

status=$(sed -n "s/.*child \"$label\" exited with exit value \([0-9][0-9]*\).*/\1/p" \
	"$log" | head -1)

if [ -z "$status" ]; then
	echo "run_component.sh: $label produced no exit status within ${limit}s" >&2
	echo "--- full log ---" >&2
	cat "$log" >&2
	rm -f "$log"
	exit 125
fi

rm -f "$log"
exit "$status"
