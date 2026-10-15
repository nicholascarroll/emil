#!/bin/bash
# Runs INSIDE the Redox guest (and anywhere with bash, for testing the
# harness).  Executes several unit-suite binaries in one VM boot.
#
# Usage: redox_batch.sh SECS suite...
#
# Each suite binary is ./test_<suite> in the current directory.  Output is
# bracketed so run_tests.sh can split it back into per-suite results:
#
#   @@BEGIN <suite>
#   ...the suite's own output, ending in its OK/FAIL line...
#   @@END <suite> <exit status>
#
# A suite that hangs gets killed after SECS seconds if the guest has
# timeout(1).


secs=$1
shift

guard=""
if [ "$secs" -gt 0 ] 2>/dev/null && command -v timeout >/dev/null 2>&1; then
    guard="timeout $secs"
fi

for s in "$@"; do
    echo "@@BEGIN $s"
    $guard "./test_$s"
    echo "@@END $s $?"
done
