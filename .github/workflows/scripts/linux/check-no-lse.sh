#!/bin/bash
# Fails if an arm64 binary carries LSE (ARMv8.1 atomics) instructions inline.
#
# The Linux arm64 builds have to run on cores without LSE (Cortex-A53-class handhelds and SBCs),
# where any cas/ldadd/swp/... is SIGILL. cmake/BuildParameters.cmake builds them with
# -march=armv8-a -moutline-atomics, so their atomics go through the compiler's __aarch64_* helpers,
# which test HWCAP_ATOMICS at runtime and use LSE only on cores that have it. Those helpers are the
# one place LSE is allowed; an LSE instruction anywhere else means the -march did not reach that
# code (a flag lost to a later -march, an LTO step that dropped the target features, a hand-written
# asm block). Run it on the unstripped binary: the helpers are found by their symbol names.
#
# usage: check-no-lse.sh <binary> [objdump]

set -euo pipefail

BIN="${1:?usage: check-no-lse.sh <binary> [objdump]}"
OBJDUMP="${2:-llvm-objdump-17}"

if [ ! -f "$BIN" ]; then
  echo "::error::check-no-lse: $BIN does not exist"
  exit 1
fi

DISASM=$(mktemp)
trap 'rm -f "$DISASM"' EXIT
"$OBJDUMP" -d --no-show-raw-insn "$BIN" > "$DISASM"

# A disassembly with no instructions in it would pass every check, so make sure there are some.
if ! grep -qE '^ *[0-9a-f]+:' "$DISASM"; then
  echo "::error::check-no-lse: $OBJDUMP produced no instructions for $BIN"
  exit 1
fi

# Function headers look like "0000000000401000 <name>:", instructions like
# "  401000:  <tab>casal<tab>w0, w1, [x2]". Report every LSE instruction outside a
# __aarch64_* outline-atomics helper, with the function it is in.
HITS=$(awk '
  /^[0-9a-f]+ <.*>:$/ {
    fn = $0
    sub(/^[0-9a-f]+ </, "", fn)
    sub(/>:$/, "", fn)
    next
  }
  /^ *[0-9a-f]+:/ {
    line = $0
    sub(/^ *[0-9a-f]+:[ \t]*/, "", line)
    split(line, f, /[ \t]+/)
    op = f[1]
    if (op ~ /^(casp?|ld(add|clr|eor|set|smax|smin|umax|umin)|st(add|clr|eor|set|smax|smin|umax|umin)|swp)(a|al|l)?(b|h)?$/ && fn !~ /^__aarch64_/)
      print fn ": " line
  }
' "$DISASM")

if [ -n "$HITS" ]; then
  COUNT=$(printf '%s\n' "$HITS" | wc -l)
  echo "::error::check-no-lse: $BIN has $COUNT inline LSE instruction(s); it will SIGILL on ARMv8.0 cores"
  printf '%s\n' "$HITS" | head -50
  exit 1
fi

HELPERS=$(grep -cE '^[0-9a-f]+ <__aarch64_' "$DISASM" || true)
echo "check-no-lse: no inline LSE instructions in $BIN (${HELPERS} outline-atomics helpers)"
