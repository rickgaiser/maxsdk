#!/bin/bash
# Checks for vuk: the examples compile for VU0 and VU1, and bad programs give
# the expected errors.
#
#   ./test.sh [<llvm bin dir>]     (default: toolchain/build/llvm/bin)

HERE=$(cd "$(dirname "$0")" && pwd)
LLVM=${1:-$(cd "$HERE/../../../../../build/llvm/bin" && pwd)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILS=0

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# Examples: vuk -> llc for both units, with the machine verifier.
for f in "$HERE"/poly*.vuk; do
  # poly4 takes 4 degrees per step: D a multiple of 4
  case "$f" in *poly4.vuk) ds="4 16" ;; *) ds="1 4 16" ;; esac
  for d in $ds; do
    python3 -I "$HERE/vuk.py" "$f" -D D=$d -o "$TMP/k.ll" -H "$TMP/k.h" || { fail "$f D=$d: vuk"; continue; }
    for cpu in vu0 vu1; do
      "$LLVM/llc" -mtriple=vu -mcpu=$cpu -O3 -verify-machineinstrs "$TMP/k.ll" -o /dev/null 2> "$TMP/err" ||
        fail "$(basename "$f") D=$d $cpu: llc: $(head -1 "$TMP/err")"
    done
  done
done

# The shaders and kernels of the demos, for their unit.
for f in "$HERE"/../../examples/*/*.vuk "$HERE"/../../demos/*/*.vuk; do
  python3 -I "$HERE/vuk.py" "$f" -o "$TMP/s.ll" -H "$TMP/s.h" || { fail "$f: vuk"; continue; }
  "$LLVM/llc" -mtriple=vu -O3 -verify-machineinstrs "$TMP/s.ll" -o /dev/null 2> "$TMP/err" ||
    fail "$(basename "$f"): llc: $(head -1 "$TMP/err")"
done

# Errors: each case is "<program>|<expected message part>".
while IFS='|' read -r prog msg; do
  [ -z "$prog" ] && continue
  printf '%s\n' "$prog" > "$TMP/e.vuk"
  out=$(python3 -I "$HERE/vuk.py" "$TMP/e.vuk" -o /dev/null 2>&1) && { fail "accepted: $prog"; continue; }
  case "$out" in *"$msg"*) ;; *) fail "$prog: got '$out', want '$msg'" ;; esac
done <<'EOF'
kernel k(x: f32x4[], n: i16) { }|an unsized array must be the last parameter
kernel k(c: f32[4], n: i16) { for i in 0..n { var y = c[i]; } }|needs a constant index
kernel k(n: i16) { y = 1; }|cannot assign to y
kernel k(n: i16) { var y = z; }|unknown name z
kernel k(n: i16) { var y = n / 2; }|integer division is not supported
kernel k(n: i16 { }|expected ,
kernel k(n: u8) { }|unknown type u8
shader s { in p: v3_32; out xyz9 = p; }|unknown GIF register xyz9
shader s { in p: v3_12; out xyz2 = p; }|unknown format v3_12
shader s { out xyz2 = 1; }|no inputs
shader s { in p: v3_32; }|no outputs
shader s { in p: v3_32; out xyz2 = foo(p); }|unknown function foo()
shader s { in p: v3_32; out xyz2 = min(p); }|min() takes 2 arguments
shader s { in p: v3_32; out xyz2 = p.q; }|unknown lane .q
unit vu0; shader s { in p: v3_32; out xyz2 = p; }|shaders run on VU1
unit vu2;|unknown unit vu2
EOF

# A shader's header: the layout of its buffer and how to send its inputs.
printf '%s\n' 'shader s { uniform m: f32x4[4]; in p: v3_32; in c: v4_8u; out rgbaq = c; out xyz2 = ftoi4(p / p.w); }' > "$TMP/s.vuk"
python3 -I "$HERE/vuk.py" "$TMP/s.vuk" -o /dev/null -H "$TMP/s.h"
for want in "VUK_S_M 1" "VUK_S_UNI_QW 4" "VUK_S_ITEM_QW 2" "VUK_S_OUT_QW 2" "VUK_S_MAX_N 125" \
            "VUK_S_ALIGN 4" "VUK_S_REGS 0x51ull" "VUK_S_IN_AT 5" "VUK_S_OUT_AT 261"; do
  grep -qE "#define $want( |$)" "$TMP/s.h" || fail "shader layout: missing $want"
done

# The layout header matches the documented layout.
python3 -I "$HERE/vuk.py" "$HERE/poly.vuk" -D D=16 -o /dev/null -H "$TMP/p.h"
for want in "VUK_POLY_N 0" "VUK_POLY_C 1" "VUK_POLY_X 6" "VUK_D 16"; do
  grep -qE "#define $want( |$)" "$TMP/p.h" || fail "layout: missing $want"
done

if [ $FAILS -eq 0 ]; then echo "vuk: all checks passed"; else echo "vuk: $FAILS failed"; exit 1; fi
