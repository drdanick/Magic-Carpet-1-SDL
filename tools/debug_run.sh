#!/usr/bin/env bash
# Run remc1 under the Windows console debugger (cdb) and print a symbolised stack trace when it
# crashes or exits. Windows / Git Bash only.
#
# Usage: tools/debug_run.sh [--build <dir>] -- <remc1 args...>
#   e.g. tools/debug_run.sh -- --data C:/mc1data --input-script s.txt --exit-after-frames 2000
#
# Notes:
#  * -hd disables the NT debug heap. Without it heap layout differs from a normal run and many
#    decompilation bugs (out-of-bounds reads into uninitialised memory) do not reproduce.
#  * Breakpoints on exit/_exit show who terminated the game (it calls exit() on many errors).
#  * The full cdb log path is printed at the end.
BUILD="build/win-x86/Debug"
if [ "$1" == "--build" ]; then BUILD="$2"; shift 2; fi
[ "$1" == "--" ] && shift
CDB="${CDB:-/c/Program Files (x86)/Windows Kits/10/Debuggers/x86/cdb.exe}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO/$BUILD" || exit 1
LOG="${DEBUG_RUN_LOG:-$(mktemp)}"
"$CDB" -hd -o -lines \
  -c "sxe av; bu ucrtbased!exit \"kP 30; q\"; bu ucrtbase!exit \"kP 30; q\"; g; .ecxr; kP 30; r; q" \
  ./remc1.exe "$@" > "$LOG" 2>&1
# show from the first exception/breakpoint onwards
grep -v -e "^ModLoad" -e "NatVis" -e "^\*\*\*\*" -e "Symbol search path" -e "^$" "$LOG" \
  | sed -n -e '/violation\|Breakpoint\|exception -\|Privileged\|Illegal\|ChildEBP/,$p' | head -120
echo "(full log: $LOG)"
