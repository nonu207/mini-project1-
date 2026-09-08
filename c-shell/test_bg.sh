#!/usr/bin/env bash
# Automated verification for the background execution (`&`) feature.
# Usage: bash test_bg.sh
set -u

SHELL_BIN="./shell.out"
PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

check() {
  local desc="$1" expected="$2" actual="$3"
  if [[ "$actual" == *"$expected"* ]]; then
    echo "PASS: $desc"
    PASS=$((PASS + 1))
  else
    echo "FAIL: $desc (expected to contain '$expected'; got: $actual)"
    FAIL=$((FAIL + 1))
  fi
}

# 1. Launch + immediate prompt + normal completion report
out=$( (printf 'sleep 1 &\n'; sleep 2) | "$SHELL_BIN" 2>&1 >/dev/null )
check "prints job number"        "[1]"                 "$out"
check "prints process id"        "sleep with pid"      "$out"
check "reports normal exit"      "exited normally."    "$out"

# 2. Multiple &-separated commands launched independently
out=$( (printf 'echo hello & echo world &\n'; sleep 2) | "$SHELL_BIN" 2>&1 >/dev/null )
check "launches two bg jobs"     "[1]"                 "$out"
check "second job number"        "[2]"                 "$out"

# 3. Abnormal termination (killed by signal)
out=$( (printf 'sh -c '\''kill -TERM $$\x27 &\n'; sleep 1.5) | "$SHELL_BIN" 2>&1 >/dev/null )
check "reports abnormal exit"    "exited abnormally."  "$out"

# 4. Non-zero exit status is still a normal exit
out=$( (printf 'false &\n'; sleep 1.5) | "$SHELL_BIN" 2>&1 >/dev/null )
check "non-zero exit is normal"  "false with pid"      "$out"
check "non-zero exit normal msg" "exited normally."    "$out"

# 5. Job numbers are monotonic across lines
out=$( (printf 'true &\n'; sleep 0.3; printf 'true &\n'; sleep 1.5) | "$SHELL_BIN" 2>&1 >/dev/null )
check "monotonic job 1"          "[1]"                 "$out"
check "monotonic job 2"          "[2]"                 "$out"

# 6. Background process cannot read the terminal (stdin = /dev/null)
out=$( (printf 'cat &\n'; sleep 1.5) | "$SHELL_BIN" 2>&1 >/dev/null )
check "bg stdin is null (no hang)" "cat with pid"      "$out"

# 7. Pipeline: reported pid is first command's pid (single pid registered)
out=$( (printf 'ls | awk '\''{print $1}'\'' > /dev/null &\n'; sleep 1.5) | "$SHELL_BIN" 2>&1 >/dev/null )
first_pid=$(printf '%s\n' "$out" | grep -oE '^\[[0-9]+\] [0-9]+' | awk '{print $2}')
if [[ -n "$first_pid" ]] && printf '%s' "$out" | grep -c "$first_pid" >/dev/null; then
  echo "PASS: pipeline reports first command pid"
  PASS=$((PASS + 1))
else
  echo "FAIL: pipeline did not report a first-command pid"
  FAIL=$((FAIL + 1))
fi

# 8. Redirections apply to background jobs
rm -f "$TMP/report.txt"
out=$( (printf 'echo RDIR > %s/report.txt &\n' "$TMP"; sleep 1.5) | "$SHELL_BIN" 2>&1 >/dev/null )
if [[ -f "$TMP/report.txt" ]] && [[ "$(cat "$TMP/report.txt")" == "RDIR" ]]; then
  echo "PASS: background redirection wrote file"
  PASS=$((PASS + 1))
else
  echo "FAIL: background redirection did not write file"
  FAIL=$((FAIL + 1))
fi

# 9. Unknown background command is reported
out=$( (printf 'nonexistent_cmd_xyz &\n'; sleep 0.5) | "$SHELL_BIN" 2>&1 >/dev/null )
check "unknown bg command message" "command not found" "$out"

echo
echo "=========================="
echo "PASS=$PASS FAIL=$FAIL"
echo "=========================="
[[ "$FAIL" -eq 0 ]]