#!/usr/bin/env bash
# Runs one already-built ELF under QEMU's mps2-an385 machine and reports
# its real exit code - the board's own SYS_EXIT_EXTENDED semihosting call
# (src/startup.c's _exit(), issue #123 Finding 1) is what makes that exit
# code meaningful (a plain SYS_EXIT can only ever report host exit code 1
# regardless of the program's own intent).
#
# Not wired through ctest/add_test() - see tests/CMakeLists.txt's own top
# comment for why. Usage: run_under_qemu.sh <path-to-elf> [timeout-seconds]
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 <path-to-elf> [timeout-seconds]" >&2
  exit 2
fi

elf="$1"
timeout_seconds="${2:-60}"

timeout "${timeout_seconds}" qemu-system-arm -M mps2-an385 -semihosting -nographic -kernel "${elf}"
