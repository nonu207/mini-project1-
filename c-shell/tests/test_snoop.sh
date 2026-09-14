#!/usr/bin/env bash
# Linux-only test for the snoop built-in.
# Usage: test_snoop.sh /abs/path/shell.out /abs/path/snoop_target
SH="$1"
TARGET="$2"
W="$(mktemp -d)"; cd "$W" || exit 1
pass=0; fail=0
chk() { local d="$1"; shift; if "$@"; then echo "PASS  $d"; pass=$((pass+1)); else echo "FAIL  $d"; fail=$((fail+1)); fi; }

mkfifo in
"$SH" < in > out 2> err &
SPID=$!
exec 3> in
send() { echo "$1" >&3; sleep "${2:-0.6}"; }
last_err() { tail -1 err; }
# Summary rows of a file: everything after the header line.
rows() { awk 'f{print} /^syscall +calls +time$/{f=1}' "$1"; }
calls_of() { rows "$1" | awk -v n="$2" '$1==n{print $2}'; }
# Checks below run in `bash -c`, which only sees exported functions.
export -f rows calls_of

# ── 1. snoop sleep 1 (the spec example) ─────────────────────────────────
send "snoop sleep 1 > sleep.txt" 2
echo "---- snoop sleep 1 ----"; cat sleep.txt; echo "----"
chk "header matches the spec"           grep -qx "syscall       calls   time" sleep.txt
SLEEPCALL=$(rows sleep.txt | awk '$1=="nanosleep"||$1=="clock_nanosleep"{print $1}' | head -1)
chk "a nanosleep row exists ($SLEEPCALL)" [ -n "$SLEEPCALL" ]
chk "  it was called once"              [ "$(calls_of sleep.txt "$SLEEPCALL")" = 1 ]
chk "summary has several rows"          [ "$(rows sleep.txt | wc -l)" -gt 3 ]
chk "  and took ~1s (0.95–1.20)"        bash -c "rows sleep.txt | awk '\$1==\"$SLEEPCALL\"{t=\$3; sub(/s\$/,\"\",t); ok=(t>=0.95 && t<=1.20)} END{exit !ok}'"
chk "exit_group: 1 call, 0.000s"        bash -c "rows sleep.txt | awk '\$1==\"exit_group\" && \$2==1 && \$3==\"0.000s\"{f=1} END{exit !f}'"
chk "counts are non-increasing"         bash -c "rows sleep.txt | awk '{print \$2}' | sort -rnc && [ \$(rows sleep.txt | wc -l) -gt 0 ]"
chk "each syscall listed once"          bash -c "[ \$(rows sleep.txt | wc -l) -gt 0 ] && [ -z \"\$(rows sleep.txt | awk '{print \$1}' | sort | uniq -d)\" ]"
chk "time column has 3 decimals + s"    bash -c "rows sleep.txt | awk '\$3 !~ /^[0-9]+\\.[0-9][0-9][0-9]s\$/{bad=1} END{exit bad || NR==0}'"

# ── 2. known profile: ordering, ties, unknown numbers ─────────────────────
send "snoop $TARGET > target.txt" 1
echo "---- snoop snoop_target ----"; cat target.txt; echo "----"
line_of() { rows target.txt | awk -v n="$1" '$1==n{print NR}'; }
chk "getuid counted 5 times"            [ "$(calls_of target.txt getuid)" = 5 ]
chk "getppid counted 3 times"           [ "$(calls_of target.txt getppid)" = 3 ]
chk "getpid counted 3 times"            [ "$(calls_of target.txt getpid)" = 3 ]
chk "higher count listed first (getuid above getppid)" [ "$(line_of getuid)" -lt "$(line_of getppid)" ]
chk "tie broken by first occurrence (getppid above getpid)" [ "$(line_of getppid)" -lt "$(line_of getpid)" ]
chk "unknown syscall printed as syscall_1000" [ "$(calls_of target.txt syscall_1000)" = 1 ]

# ── 3. arguments are passed, output still appears ─────────────────────────
send "snoop echo hello world > echo.txt" 1
chk "command's own output comes through" [ "$(head -1 echo.txt)" = "hello world" ]
chk "write counted"                      [ -n "$(calls_of echo.txt write)" ]

# ── 4. counts identical to strace ─────────────────────────────────────────
if command -v strace >/dev/null; then
  send "snoop ls -la / > ls.txt" 1
  strace -qq -o ls.strace ls -la / > ls_plain.txt
  # snoop starts counting after the initial execve, as strace -c does not.
  grep -v '^+++\|^---' ls.strace | sed -E 's/^([a-z0-9_]+)\(.*/\1/' |
    grep -E '^[a-z0-9_]+$' | grep -vx execve | sort | uniq -c |
    awk '{print $2, $1}' | sort > strace_counts.txt
  rows ls.txt | awk '{print $1, $2}' | sort > snoop_counts.txt
  chk "per-syscall counts identical to strace (ls -la /)" diff strace_counts.txt snoop_counts.txt
  diff strace_counts.txt snoop_counts.txt | head -10
fi

# ── 5. errors ─────────────────────────────────────────────────────────────
send "snoop -p 99999999";           chk "snoop -p <missing pid>"     [ "$(last_err)" = "snoop: no such process" ]
send "snoop -p 4000000";            chk "snoop -p <unused pid>"      [ "$(last_err)" = "snoop: no such process" ]
send "snoop nosuchcommand_xyz";     chk "unknown command"            [ "$(last_err)" = "snoop: command not found" ]
send "snoop";                       chk "no arguments"               [ "$(last_err)" = "snoop: invalid syntax" ]
send "snoop -p";                    chk "-p without pid"             [ "$(last_err)" = "snoop: invalid syntax" ]
send "snoop -p abc";                chk "-p non-numeric"             [ "$(last_err)" = "snoop: invalid syntax" ]
send "snoop -p 1 2";                chk "-p with two pids"           [ "$(last_err)" = "snoop: invalid syntax" ]

# ── 6. -p on a running background job, until it exits ─────────────────────
send "sleep 2 &"
P=$(grep -o '\[[0-9]*\] [0-9]*' out | tail -1 | awk '{print $2}')
send "snoop -p $P > attach.txt" 2.5
echo "---- snoop -p (sleep 2) ----"; cat attach.txt; echo "----"
chk "attach: summary printed"           grep -qx "syscall       calls   time" attach.txt
chk "attach: exit_group seen"           [ "$(calls_of attach.txt exit_group)" = 1 ]
chk "attach: job's completion still announced" grep -q "sleep with pid $P exited normally" attach.txt out
send "activities > act.txt"
chk "attach: job no longer listed by activities" bash -c "! grep -q ' $P ' act.txt"

# ── 7. -p then Ctrl-C: detach, process keeps running ──────────────────────
send "sleep 100 &"
Q=$(grep -o '\[[0-9]*\] [0-9]*' out | tail -1 | awk '{print $2}')
send "snoop -p $Q > detach.txt" 1
kill -INT "$SPID"; sleep 1
chk "Ctrl-C: summary printed"           grep -qx "syscall       calls   time" detach.txt
chk "Ctrl-C: process still alive"       kill -0 "$Q"
chk "Ctrl-C: no longer traced (TracerPid 0)" grep -qx $'TracerPid:\t0' /proc/$Q/status
chk "Ctrl-C: not left stopped"          bash -c "! grep -q '^State:.*T' /proc/$Q/status"
kill "$Q"

send "echo still-alive"
chk "shell still works afterwards"      grep -q "still-alive" out

exec 3>&-
wait "$SPID" 2>/dev/null
echo; echo "passed=$pass failed=$fail"
