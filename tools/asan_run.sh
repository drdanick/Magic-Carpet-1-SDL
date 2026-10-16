#!/usr/bin/env bash
# Build the AddressSanitizer configuration and run remc1 with the given arguments; print the first
# ASan report (stack + "located" line), or "no ASan errors". Windows / Git Bash.
#
# Usage: tools/asan_run.sh [--no-build] -- <remc1 args...>
#   e.g. tools/asan_run.sh -- --data C:/mc1data --input-script s.txt --exit-after-frames 1500
# This is the main tool for finding decompilation memory bugs.
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=1
if [ "$1" == "--no-build" ]; then BUILD=0; shift; fi
[ "$1" == "--" ] && shift
if [ $BUILD == 1 ]; then
  (cd "$REPO" && ./build.sh win-x86-asan > "${TMPDIR:-/tmp}/remc1_asan_build.log" 2>&1) || {
    grep -E ": (fatal )?error" "${TMPDIR:-/tmp}/remc1_asan_build.log" | sort -u | head -20; exit 2; }
fi
LOG="${ASAN_RUN_LOG:-$(mktemp)}"
cd "$REPO/build/win-x86-asan/Debug" || exit 1
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" ./remc1.exe "$@" > "$LOG" 2>&1
CODE=$?
if grep -q "ERROR: AddressSanitizer" "$LOG"; then
  grep -A16 "ERROR: AddressSanitizer" "$LOG" | grep -v "^Shadow\|^  0x" | head -18
  grep -m1 -A1 "is located" "$LOG"
else
  echo "no ASan errors (exit code $CODE)"
  tail -3 "$LOG"
fi
echo "(log: $LOG)"
