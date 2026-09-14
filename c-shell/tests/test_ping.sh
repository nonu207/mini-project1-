#!/usr/bin/env bash
# Drives shell.out through a FIFO so we can read real pids and then ping them.
SHELL_BIN="$1"
DIR="$(mktemp -d)"
FIFO="$DIR/in"; OUT="$DIR/out"
mkfifo "$FIFO"
"$SHELL_BIN" < "$FIFO" > "$OUT" 2>&1 &
SH=$!
exec 3> "$FIFO"

send() { echo "$1" >&3; sleep 0.4; }
pass=0; fail=0
expect_last() {   # expect_last "<description>" "<expected line>"
  local got; got="$(grep -v '^$' "$OUT" | sed 's/^<[^>]*> *//' | grep -v '^$' | tail -1)"
  if [[ "$got" == "$2" ]]; then echo "PASS  $1  -> $got"; pass=$((pass+1))
  else echo "FAIL  $1  expected [$2] got [$got]"; fail=$((fail+1)); fi
}
check() {         # check "<description>" <command...>
  local d="$1"; shift
  if "$@"; then echo "PASS  $d"; pass=$((pass+1)); else echo "FAIL  $d"; fail=$((fail+1)); fi
}

# Job 1: a plain sleep.  Job 2: a pipeline (stand-in for "cat | sort &";
# background jobs get /dev/null as stdin here, so cat would exit at once).
send "sleep 100 &"
P1=$(grep -o '\[1\] [0-9]*' "$OUT" | awk '{print $2}' | tail -1)
send "sleep 100 | sort &"
P2=$(grep -o '\[2\] [0-9]*' "$OUT" | awk '{print $2}' | tail -1)
echo "job1 pid=$P1  job2 lead pid=$P2"

alive() { kill -0 "$1" 2>/dev/null && [[ "$(ps -o stat= -p "$1" | tr -d ' ')" != Z* ]]; }
state() { ps -o stat= -p "$1" | tr -d ' '; }

# --- spec examples ---------------------------------------------------
send "ping $P1 0";      expect_last "ping pid 0 (probe only)"   "Sent signal 0 to $P1"
check "  sleep still alive after signal 0" alive "$P1"
send "ping $P2 64";     expect_last "ping pid 64 (64%64=0)"      "Sent signal 64 to $P2"
check "  job2 still alive after 64" alive "$P2"
send "ping 99999 9";    expect_last "unknown pid"                "ping: no such process found"
send "ping %5 9";       expect_last "unknown job"                "ping: no such process found"
send "ping $P1 abc";    expect_last "non-numeric signal"         "ping: invalid syntax"
send "ping $P1 -3";     expect_last "negative signal"            "ping: invalid syntax"

# --- validation order / edge cases ------------------------------------
send "ping 99999 abc";  expect_last "bad signal beats bad target" "ping: invalid syntax"
send "ping %5 -1";      expect_last "bad signal beats bad job"   "ping: invalid syntax"
send "ping 1 9";        expect_last "system pid not ours (launchd/init)" "ping: no such process found"
send "ping $$ 0";       expect_last "real pid not spawned by shell" "ping: no such process found"
send "ping abc 9";      expect_last "garbage target"             "ping: no such process found"
send "ping % 9";        expect_last "bare %"                     "ping: no such process found"
send "ping $P1";        expect_last "missing signal"             "ping: invalid syntax"
send "ping";            expect_last "no args"                    "ping: invalid syntax"
send "ping $P1 +9";     expect_last "plus sign"                  "ping: invalid syntax"
send "ping $P1 18446744073709551680"
                        expect_last "huge number, no overflow (mod 64 = 0)" "Sent signal 18446744073709551680 to $P1"
check "  sleep still alive after huge (=0)" alive "$P1"

# --- stop / continue via job, reflected in activities ------------------
STOP=$(kill -l STOP >/dev/null 2>&1; [[ "$(uname)" == Darwin ]] && echo 17 || echo 19)
CONT=$(       [[ "$(uname)" == Darwin ]] && echo 19 || echo 18)
send "ping %2 $STOP";   expect_last "stop job 2 by %"            "Sent signal $STOP to %2"
check "  job2 lead is stopped (ps state T)" bash -c "[[ \$(ps -o stat= -p $P2) == *T* ]]"
send "activities";      check "  activities shows Stopped" grep -q "$P2 sleep Stopped" "$OUT"
send "ping %2 $CONT";   expect_last "continue job 2"             "Sent signal $CONT to %2"
check "  job2 lead running again" bash -c "[[ \$(ps -o stat= -p $P2) != *T* ]]"

# --- actually delivering a kill: 79 % 64 = 15 (SIGTERM) ----------------
send "ping $P1 79";     sleep 0.5
check "ping pid 79 printed original number" grep -q "Sent signal 79 to $P1" "$OUT"
check "  sleep got SIGTERM and is gone" bash -c "! kill -0 $P1 2>/dev/null"
send "ping $P1 9";      expect_last "pid no longer tracked after exit" "ping: no such process found"

send "ping %2 9";       sleep 0.5
check "ping %2 9 printed" grep -q "Sent signal 9 to %2" "$OUT"
check "  whole job2 group killed" bash -c "! kill -0 $P2 2>/dev/null"
send "ping %2 9";       expect_last "job gone after kill"        "ping: no such process found"

# --- ping inside a pipeline / background still works -------------------
send "sleep 100 &"
P3=$(grep -o '\[3\] [0-9]*' "$OUT" | awk '{print $2}' | tail -1)
send "ping %3 15 | cat";  sleep 0.3
check "ping in pipeline (child copy of table)" grep -q "Sent signal 15 to %3" "$OUT"
check "  job3 killed" bash -c "! kill -0 $P3 2>/dev/null"

exec 3>&-
wait $SH 2>/dev/null
echo; echo "---- raw shell transcript ----"; cat "$OUT"
echo; echo "passed=$pass failed=$fail"
rm -rf "$DIR"
