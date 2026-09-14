#!/usr/bin/env bash
# Linux-only test for the spy built-in.  Usage: test_spy.sh /abs/path/shell.out
SH="$1"
W="$(mktemp -d)"; cd "$W" || exit 1
pass=0; fail=0
chk() { local d="$1"; shift; if "$@"; then echo "PASS  $d"; pass=$((pass+1)); else echo "FAIL  $d"; fail=$((fail+1)); fi; }

mkfifo in
"$SH" < in > out 2> err &
SPID=$!
exec 3> in
send() { echo "$1" >&3; sleep 0.6; }

# ── 1. no argument: the shell itself (stdout redirected to a file) ──────
send "spy > self.txt"
S=self.txt
chk "header is exact"                 [ "$(head -1 $S)" = "PID    FD    TYPE   PATH" ]
chk "every row has the shell's pid"   bash -c "tail -n +2 $S | awk '{print \$1}' | sort -u | grep -qx $SPID && [ \$(tail -n +2 $S | awk '{print \$1}' | sort -u | wc -l) = 1 ]"
chk "cwd row: DIR + working dir"      grep -qx "$SPID    cwd    DIR    $W" $S
chk "txt row: REG + shell binary"     grep -qx "$SPID    txt    REG    $(readlink -f "$SH")" $S
chk "mem includes libc"               grep -Eq "^$SPID    mem    REG    /.*libc\.so" $S
chk "mem paths are unique"            bash -c "[ -z \"\$(awk '\$2==\"mem\"{print \$4}' $S | sort | uniq -d)\" ]"
chk "executable not repeated as mem"  bash -c "! awk '\$2==\"mem\"{print \$4}' $S | grep -qx '$(readlink -f "$SH")'"
chk "no pseudo mappings ([heap] etc)" bash -c "! grep -q '\[' $S"
chk "fd 0 is FIFO (stdin is a fifo)"  grep -qx "$SPID    0      FIFO   $W/in" $S
chk "fd 1 is REG self.txt (redirect)" grep -qx "$SPID    1      REG    $W/self.txt" $S
chk "fd 2 is REG err"                 grep -qx "$SPID    2      REG    $W/err" $S
chk "row order cwd,txt,mem...,fds"    bash -c "tail -n +2 $S | awk '{print \$2}' | sed 's/^[0-9]*\$/N/' | uniq | tr '\n' ' ' | grep -qx 'cwd txt mem N '"
chk "numeric fds ascending"           bash -c "awk '\$2 ~ /^[0-9]+\$/{print \$2}' $S | sort -nc"
chk "spy's own /proc/<pid>/fd handle not listed" bash -c "! grep -q '/proc/' $S"

# ── 2. a given pid: a background sleep ─────────────────────────────────
send "sleep 100 &"
P=$(grep -o '\[1\] [0-9]*' out | awk '{print $2}' | tail -1)
send "spy $P > sleep.txt"
T=sleep.txt
echo "---- spy $P ----"; cat $T; echo "----"
chk "pid column is the sleep's pid"   bash -c "[ \"\$(tail -n +2 $T | awk '{print \$1}' | sort -u)\" = $P ]"
chk "txt REG /usr/bin/sleep"          grep -qx "$P    txt    REG    $(readlink -f /proc/$P/exe)" $T
chk "cwd DIR"                         grep -qx "$P    cwd    DIR    $W" $T
chk "fd 0 CHR /dev/null (bg stdin)"   grep -qx "$P    0      CHR    /dev/null" $T
chk "bg job has only fds 0,1,2 (no leaked 3/4)" bash -c "[ \"\$(awk '\$2 ~ /^[0-9]+\$/{print \$2}' $T | tr '\n' ' ')\" = '0 1 2 ' ]"

# ── 3. cross-check against real lsof ───────────────────────────────────
if command -v lsof >/dev/null; then
  lsof -p "$P" -F ftn 2>/dev/null | awk '
    /^f/{fd=substr($0,2)} /^t/{ty=substr($0,2)}
    /^n/{ if (fd=="cwd"||fd=="txt"||fd=="mem"||fd ~ /^[0-9]+$/) print fd, ty, substr($0,2) }' \
    | sed 's/ (path dev=.*)$//' | sort -u > lsof.txt
  tail -n +2 $T | awk '{print $2, $3, $4}' | sort -u > spy.txt
  chk "identical to lsof -p (fd,type,path)" diff lsof.txt spy.txt
  diff lsof.txt spy.txt | head
fi

# ── 4. errors ──────────────────────────────────────────────────────────
last_err() { tail -1 err; }
send "spy 1 2";          chk "two pids -> invalid syntax"      [ "$(last_err)" = "spy: invalid syntax" ]
send "spy $P $P $P";     chk "three pids -> invalid syntax"    [ "$(last_err)" = "spy: invalid syntax" ]
send "spy 99999999";     chk "missing pid -> no such process"  [ "$(last_err)" = "spy: no such process" ]
send "spy 0";            chk "pid 0 -> no such process"        [ "$(last_err)" = "spy: no such process" ]
send "spy 99999999999999999999"; chk "huge pid -> no such process" [ "$(last_err)" = "spy: no such process" ]
send "spy abc";          chk "non-numeric -> invalid syntax"   [ "$(last_err)" = "spy: invalid syntax" ]
send "spy -5";           chk "negative -> invalid syntax"      [ "$(last_err)" = "spy: invalid syntax" ]
send "kill $P";          sleep 0.3
send "spy $P";           chk "after process dies -> no such process" [ "$(last_err)" = "spy: no such process" ]

# ── 5. inside a pipeline: still reports the SHELL, not the forked child ──
send "spy | cat > piped.txt"
chk "spy | cat reports the shell pid" grep -q "^$SPID    cwd    DIR" piped.txt

exec 3>&-
wait $SPID 2>/dev/null

# ── 6. real terminal: fd 0 should be CHR /dev/pts/N like the spec example ─
if command -v script >/dev/null; then
  printf 'spy\n' | timeout 5 script -qec "$SH" /dev/null > tty.txt 2>&1
  tr -d '\r' < tty.txt > tty_clean.txt
  echo "---- spy under a pty ----"; grep -E '^[0-9]+ +(cwd|txt|0|1|2) ' tty_clean.txt; echo "----"
  chk "under a pty, fd 0 is CHR /dev/pts/N" grep -Eq '^[0-9]+    0      CHR    /dev/pts/[0-9]+$' tty_clean.txt
fi

echo; echo "---- spy (shell itself) ----"; cat self.txt
echo; echo "passed=$pass failed=$fail"
