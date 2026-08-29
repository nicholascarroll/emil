#!/usr/bin/env bash
# Fetch everything needed to read a CI run, into ./tempy.
#
# Usage: ./ci_fetch.sh [run-id]        (defaults to the latest run)
#
# Every section reports why it is empty rather than just being empty.
# The previous version piped gh's errors into the stream it then
# grepped, so an in-progress run produced a file of blank headings that
# read like a clean result.

RUN=${1:-}
OUT=tempy
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
: > "$OUT"

command -v gh >/dev/null || { echo "gh not on PATH" >&2; exit 1; }

if [ -z "$RUN" ]; then
  RUN=$(gh run list --limit 1 --json databaseId --jq '.[0].databaseId')
  [ -n "$RUN" ] || { echo "could not find a recent run" >&2; exit 1; }
fi

say() { printf '\n===== %s =====\n' "$*" >> "$OUT"; }

STATUS=$(gh run view "$RUN" --json status --jq .status 2>/dev/null)

{
  printf '\n===== run =====\n'
  gh run view "$RUN" --json databaseId,displayTitle,headSha,status,createdAt \
    --jq '[.databaseId,.displayTitle,.headSha,.status,.createdAt]|@tsv' 2>&1
} >> "$OUT"

if [ "$STATUS" != "completed" ]; then
  {
    printf '\n***** RUN IS NOT COMPLETE (status: %s) *****\n' "$STATUS"
    printf 'Job logs cannot be fetched until it finishes, so the log\n'
    printf 'sections below will say so rather than appear clean.\n'
    printf 'Conclusions are accurate for the jobs that have ended.\n'
  } >> "$OUT"
fi

gh run view "$RUN" --json jobs \
  --jq '.jobs[] | [.databaseId, (.conclusion // "running"), .name] | @tsv' \
  > "$TMP/jobs.tsv" 2>"$TMP/err"
if [ ! -s "$TMP/jobs.tsv" ]; then
  say "could not list jobs"
  cat "$TMP/err" >> "$OUT"
  echo "wrote $OUT (job list failed)"
  exit 1
fi

say "conclusions"
awk -F'\t' '{printf "%-12s %s\n", $2, $3}' "$TMP/jobs.tsv" | sort -k2 >> "$OUT"

# Fetch a job's log once.  Records why it is empty when it is empty.
joblog() {
  name=$1
  f="$TMP/$(printf '%s' "$name" | tr -c 'A-Za-z0-9' _).log"
  if [ ! -f "$f" ]; then
    id=$(awk -F'\t' -v n="$name" '$3==n {print $1; exit}' "$TMP/jobs.tsv")
    if [ -z "$id" ]; then
      printf '@@NOLOG@@ no job named %s in this run\n' "$name" > "$f"
    else
      gh run view "$RUN" --job="$id" --log > "$f.raw" 2>"$f.err"
      if [ -s "$f.raw" ]; then
        sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\t[0-9]{4}-[0-9-]*T[0-9:.]*Z /\t/' \
          "$f.raw" > "$f"
      else
        printf '@@NOLOG@@ %s\n' \
          "$(tr '\n' ' ' < "$f.err" | cut -c1-200)" > "$f"
      fi
      rm -f "$f.raw" "$f.err"
    fi
  fi
  cat "$f"
}

# Grep a job's log, and say something useful when nothing matches.
scan() {
  name=$1; pattern=$2
  out=$(joblog "$name")
  case "$out" in
    @@NOLOG@@*) printf '  (no log: %s)\n' "${out#@@NOLOG@@ }" >> "$OUT"; return ;;
  esac
  hits=$(printf '%s\n' "$out" | grep -E "$pattern")
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >> "$OUT"
  else
    printf '  (log fetched, %s lines, nothing matched)\n' \
      "$(printf '%s\n' "$out" | wc -l)" >> "$OUT"
  fi
}

jobnames() { awk -F'\t' '{print $3}' "$TMP/jobs.tsv"; }

# ---- scaffolding: nothing temporary should survive a finished tree ---
say "scaffolding check (should report nothing found)"
found=0
jobnames > "$TMP/names"
while IFS= read -r j; do
  out=$(joblog "$j")
  case "$out" in @@NOLOG@@*) continue ;; esac
  h=$(printf '%s\n' "$out" \
      | grep -iE 'TEMPORARY DIAGNOSTICS|Diagnostics . |preflight' | head -2)
  [ -z "$h" ] || { printf '%s: %s\n' "$j" "$h" >> "$OUT"; found=1; }
done < "$TMP/names"
[ "$found" -eq 1 ] || echo "  none found" >> "$OUT"

# ---- the cross targets ----------------------------------------------
for j in genode asterinas wasix wasix-pty-advisory redox redox-run; do
  grep -qx "$j" "$TMP/names" || continue
  say "$j: verdicts"
  scan "$j" 'suites:|EMIL_RESULT|emil said:|offending|asserted nothing|staged [0-9]+ (binaries|sources)|all checks passed|OK: |::error'
  say "$j: suite table"
  scan "$j" 'test_[a-z0-9_]+ +(ok|PASS|FAIL|known|expected|DID)'
done

# ---- every job that failed -------------------------------------------
# The gating jobs have been red for one reason long enough that a new
# failure would hide behind the old one, so print what each actually
# reported rather than the bare fact that it failed.
say "failing jobs: test output"
awk -F'\t' '$2=="failure" {print $3}' "$TMP/jobs.tsv" > "$TMP/failed"
while IFS= read -r j; do
  printf '\n--- %s ---\n' "$j" >> "$OUT"
  scan "$j" 'backup\.c rewritten|pty_input|pty_signals|FAIL \(|FAIL:|WARN|BUILD FAIL|TEST STATUS|::error'
done < "$TMP/failed"
[ -s "$TMP/failed" ] || echo "  none failed" >> "$OUT"

say "not finished, cancelled or skipped"
awk -F'\t' '$2!="success" && $2!="failure" {printf "%-12s %s\n", $2, $3}' \
  "$TMP/jobs.tsv" > "$TMP/other"
if [ -s "$TMP/other" ]; then cat "$TMP/other" >> "$OUT"; else echo "  none" >> "$OUT"; fi

printf 'wrote %s (%s lines) for run %s [status: %s]\n' \
  "$OUT" "$(wc -l < "$OUT")" "$RUN" "$STATUS"
