#!/bin/sh
# shots.sh <tutorials dir> <step.elf>...: run each step in PCSX2 with the
# software renderer and convert the picture it saves (host:<step>.tga) into
# <tutorial>/images/<step>.png. PCSX2_RUN is the runner: it runs an ELF in
# PCSX2 and stops it at the end marker (@@PS2TEST_END@@), with the ELF and
# its host: files in PCSX2_DIR.
set -e
here=$1
shift
PS2MAX=${PS2MAX:-$(cd "$here/../../.." && pwd)}
PCSX2_RUN=${PCSX2_RUN:-$HOME/.claude/skills/ps2-pcsx2-test/run.sh}
PCSX2_DIR=${PCSX2_DIR:-$PS2MAX/build/pcsx2-test}
for elf in "$@"; do
  step=$(basename "$elf" .elf)
  tut=$(basename "$(dirname "$elf")")
  rm -f "$PCSX2_DIR/$step.tga"
  PCSX2_RENDERER=sw "$PCSX2_RUN" "$elf" 120 > /dev/null 2>&1 || {
    echo "shots.sh: $tut/$step failed" >&2
    exit 1
  }
  [ -f "$PCSX2_DIR/$step.tga" ] || continue # a step without a picture
  mkdir -p "$here/$tut/images"
  python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).convert("RGB").save(sys.argv[2])' \
    "$PCSX2_DIR/$step.tga" "$here/$tut/images/$step.png"
  echo "$tut/images/$step.png"
done
