#!/usr/bin/env python3
"""Turn the backtrace of a maxsdk exception report into functions and lines."""

# The exception handler (src/base/exception.c) ends its report with
#   trace: <EPC> <ra> <return addresses found on the stack...>
# This prints those addresses as functions and source lines, using
# llvm-symbolizer on the program's ELF (build with EXTRA_CFLAGS=-g for file
# and line; without it only the function names are known).
#
#   tools/symbolize.py <elf> [log]      (the log defaults to stdin)
#
# The EPC is the faulting instruction. The other addresses are return
# addresses: they are looked up at address - 8, the call (jal/jalr) before
# the delay slot. Stack entries are found by a heuristic, so a frame can be
# stale; "ra" is only a caller if the faulting function had not saved it.

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PS2MAX = Path(__file__).resolve().parents[3]


def find_symbolizer():
    """Return the path of llvm-symbolizer, preferring our own LLVM build."""
    candidates = [os.environ.get("LLVM_SYMBOLIZER", "")]
    if os.environ.get("LLVM"):
        candidates.append(os.path.join(os.environ["LLVM"], "llvm-symbolizer"))
    candidates += [str(PS2MAX / "build/toolchain/generic/llvm/feature/bin/llvm-symbolizer"),
                   str(PS2MAX / "install/llvm/bin/llvm-symbolizer")]
    for c in candidates:
        if c and os.access(c, os.X_OK):
            return c
    return shutil.which("llvm-symbolizer")


def symbolize(symbolizer, elf, addresses):
    """Return, per address, a list of (function, file, line) frames, inlined first."""
    out = subprocess.run([symbolizer, "--obj=" + elf, "--inlines", "--output-style=JSON",
                          "--relativenames"] + ["0x%x" % a for a in addresses],
                         capture_output=True, text=True, check=True).stdout
    entries = []
    for line in out.splitlines():
        data = json.loads(line)
        entries += data if isinstance(data, list) else [data]
    result = []
    for entry in entries:
        frames = [(f.get("FunctionName") or "??", f.get("FileName") or "", f.get("Line") or 0)
                  for f in entry.get("Symbol", [])]
        result.append(frames or [("??", "", 0)])
    return result


def main():
    """Read the log, find the trace line, print the backtrace."""
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: symbolize.py <elf> [log]")
    elf = sys.argv[1]
    log = open(sys.argv[2], errors="replace").read() if len(sys.argv) == 3 else sys.stdin.read()
    # Any prefix: emulators and loggers put timestamps in front of the lines.
    m = re.search(r"\btrace:((?:[ \t]+[0-9a-fA-F]{8})+)[ \t\r]*$", log, re.MULTILINE)
    if not m:
        sys.exit("symbolize.py: no 'trace:' line from the exception report in the log")
    addrs = [int(a, 16) for a in m.group(1).split()]
    symbolizer = find_symbolizer()
    if not symbolizer:
        sys.exit("symbolize.py: llvm-symbolizer not found (set LLVM_SYMBOLIZER)")

    # The faulting instruction, then the calls the return addresses come from.
    # Drop repeats: ra is usually also on the stack.
    kinds, lookups, seen = [], [], set()
    for i, a in enumerate(addrs):
        if i > 0 and (a in seen or a < 8):
            continue
        seen.add(a)
        kinds.append("fault" if i == 0 else "ra" if i == 1 else "stack")
        lookups.append(a if i == 0 else a - 8)

    print("backtrace (%s):" % os.path.basename(elf))
    for n, (kind, a, frames) in enumerate(zip(kinds, lookups,
                                               symbolize(symbolizer, elf, lookups))):
        for depth, (func, file, line) in enumerate(frames):
            where = " at %s:%d" % (file, line) if file and line else ""
            inl = " (inlined)" if depth < len(frames) - 1 else ""
            print("  #%-2d %08x %-5s %s%s%s" % (n, a, kind if depth == 0 else "",
                                              func, where, inl))


if __name__ == "__main__":
    main()
