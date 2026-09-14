#!/usr/bin/env bash
# Build, run, and test the c-shell inside Linux via Docker.
#
#   ./linux.sh           build, then open the shell (Ctrl-D to leave)
#   ./linux.sh build     compile with the exact assignment flags (+ an -O2 pass)
#   ./linux.sh test      build, then run tests/test_ping.sh and tests/test_spy.sh
#   ./linux.sh bash      a Linux bash prompt with shell.out already built
#
# The project is mounted read-only and copied to /build inside the
# container, so your local files (including a macOS shell.out) are untouched.
set -euo pipefail

IMAGE=cshell-linux
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! docker info >/dev/null 2>&1; then
  echo "Docker is not running. Start Docker Desktop and try again." >&2
  exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building the $IMAGE image (first run only)..."
  docker build -t "$IMAGE" "$DIR"
fi

FLAGS='-std=c23 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror -Wno-unused-parameter -fno-asm'
BUILD="cp -r /src/. /build && rm -f shell.out && gcc $FLAGS -Iinclude src/*.c -o shell.out"

run() {
  # Always -i so piped input reaches the shell; -t only for a real terminal.
  local tty=(-i)
  [ -t 0 ] && tty=(-it)
  docker run --rm "${tty[@]}" -v "$DIR":/src:ro "$IMAGE" bash -c "$1"
}

case "${1:-shell}" in
  shell) run "$BUILD && exec ./shell.out" ;;
  build) run "$BUILD && echo 'exact flags: OK' && gcc -O2 $FLAGS -Iinclude src/*.c -o /tmp/o2.out && echo '-O2: OK'" ;;
  test)  run "$BUILD && echo '== ping ==' && bash tests/test_ping.sh /build/shell.out | grep -E '^(PASS|FAIL|passed)' && echo '== spy ==' && bash tests/test_spy.sh /build/shell.out | grep -E '^(PASS|FAIL|passed)'" ;;
  bash)  run "$BUILD && exec bash" ;;
  *)     echo "usage: $0 [shell|build|test|bash]" >&2; exit 2 ;;
esac
