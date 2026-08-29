#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Run the unit suites inside Asterinas and report.  This runs in the
# guest, as the init process, on busybox ash.
#
# Every suite runs in one boot: a QEMU boot of Asterinas is seconds, and
# 34 of them would dominate the job.  The trade-off is accepted
# knowingly -- one suite that wedges the kernel takes the rest with it,
# and the symptom is a truncated table rather than a named failure.
#
# A suite's exit status is its verdict.  TEST_END() returns non-zero on
# any failure and on a run of zero tests, so there is nothing here to
# decide and nothing to parse: run it, and print what it said if it
# failed.  Nothing is excused.
#
# Output is parsed from the QEMU serial log by CI, so two markers are
# load-bearing: a completed run prints exactly one EMIL_RESULT line, and
# nothing else in the log begins with EMIL_.

set -u

payload=${PAYLOAD_DIR:-/emil}
tests="$payload/tests"

echo ""
echo "=== emil on Asterinas ==="
echo "uname: $(uname -a 2>/dev/null || echo unavailable)"
echo ""

# Does the editor run at all?  If the binary cannot exec, everything
# below is noise and the cause is the payload rather than the editor.
version_out=$("$payload/emil" --version 2>&1)
version_rc=$?
if [ "$version_rc" -ne 0 ]; then
	echo "EMIL_RESULT: FAIL (emil --version exited $version_rc: $version_out)"
	exit 1
fi
echo "  emil --version: $version_out"
echo ""

total=0
failed=0
failed_names=''

for bin in "$tests"/test_*; do
	[ -f "$bin" ] || continue
	suite=$(basename "$bin")
	printf '  %-22s ' "$suite"
	output=$("$bin" 2>&1)
	rc=$?
	total=$((total + 1))
	if [ "$rc" -eq 0 ]; then
		# The count is display only, but a suite that quietly
		# shrinks looks identical to one that passes without it.
		echo "PASS ($(echo "$output" | awk '/Tests/{print $1; exit}') tests)"
	else
		echo "FAIL (exit $rc)"
		echo "$output" | head -40 | sed 's/^/      /'
		failed=$((failed + 1))
		failed_names="$failed_names $suite"
	fi
done

# The terminal test, gated like everything else.  Asterinas has a pty
# subsystem with termios and a line discipline, so this is the only
# place emil is driven through a terminal on a kernel that is not Linux,
# macOS or a BSD.  It reports its own failure count as its exit status.
if [ -x "$tests/pty_input_test" ]; then
	printf '  %-22s ' pty_input_test
	# Captured, not piped: $? after a pipeline is the last command's
	# status, so piping into tail would report tail's 0.
	pty_out=$("$tests/pty_input_test" "$payload/emil" 2>&1)
	pty_rc=$?
	total=$((total + 1))
	if [ "$pty_rc" -eq 0 ]; then
		echo "PASS ($(echo "$pty_out" | grep -c 'PASS$') scenarios)"
	else
		echo "FAIL (exit $pty_rc)"
		echo "$pty_out" | head -40 | sed 's/^/      /'
		failed=$((failed + 1))
		failed_names="$failed_names pty_input_test"
	fi
fi

echo ""
echo "  $total suites: $((total - failed)) passed, $failed failed"
[ "$failed" -eq 0 ] || echo "  failed:$failed_names"

# Nothing is compiled out for this target, so every suite that exists
# should have been staged.  The floor only has to catch an empty
# payload: a staging step that shipped nothing would otherwise print a
# clean tally of zeroes and report success.
if [ "$total" -lt 30 ]; then
	echo "  only $total suites ran; expected at least 30."
	echo "  Nothing was proven; treat this as a staging failure."
	echo "EMIL_RESULT: FAIL"
elif [ "$failed" -eq 0 ]; then
	echo "EMIL_RESULT: PASS"
else
	echo "EMIL_RESULT: FAIL"
fi


# Asterinas's AUTO_TEST=boot check greps the tail of the log for this
# line.  It is printed unconditionally: CI reads EMIL_RESULT for the
# real verdict, and letting make fail here too would report "Boot test
# failed", which names the wrong thing.
echo "Successfully booted."
