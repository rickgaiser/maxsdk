#!/usr/bin/env python3
"""Turn mx_trace_dump() output ("T <count> <event> <arg>" lines in a log) into
a timeline and per-unit busy/idle numbers.

    python3 -I tools/trace.py <log>

For each DMA channel: segments (DMA_START to the next DMAC_IRQ of that
channel), their busy time, and the idle gaps between an end interrupt and
the next start. Times are EE cycles (294.912 MHz). MX_TR_SAMPLE events
(mx_trace_sample) give the busy share of every unit.
"""
import sys

EV = {1: "DMAC_IRQ", 2: "INTC_IRQ", 3: "DMA_START", 4: "DMA_END", 5: "WAIT_BEGIN",
      6: "WAIT_END", 7: "GS_SIGNAL", 8: "GS_FINISH", 9: "VSYNC", 10: "SAMPLE"}
UNITS = ["VIF0 DMA", "VIF1 DMA", "GIF DMA", "fromSPR", "toSPR", "VU0", "VU1", "GIF path",
         "GS FIFO"]


def main(path):
    evs = []
    for line in open(path, errors="replace"):
        parts = line.split()
        i = parts.index("T") if "T" in parts else -1
        if i < 0 or len(parts) < i + 4:
            continue
        try:
            evs.append((int(parts[i + 1]), int(parts[i + 2]), int(parts[i + 3])))
        except ValueError:
            continue
    if not evs:
        print("no trace lines")
        return
    t0 = evs[0][0]
    # unwrap the 32-bit counter
    out, base, last = [], 0, evs[0][0]
    for t, e, a in evs:
        if t < last:
            base += 1 << 32
        last = t
        out.append((t + base - t0, e, a))
    samples = [a for _, e, a in out if e == 10]
    if samples:
        print(f"{len(samples)} samples:")
        for i, name in enumerate(UNITS):
            n = sum(1 for a in samples if a >> i & 1)
            print(f"  {name:9s} busy {100 * n // len(samples):3d}%")
        out = [x for x in out if x[1] != 10]
        if not out:
            return
    for t, e, a in out[:40]:
        print(f"{t:10d}  {EV.get(e, e):10s} {a}")
    if len(out) > 40:
        print(f"... {len(out) - 40} more")
    span = out[-1][0] - out[0][0] or 1
    for ch in sorted({a for _, e, a in out if e in (1, 3)}):
        start = None
        busy = gaps = n = 0
        last_end = None
        for t, e, a in out:
            if a != ch:
                continue
            if e == 3:
                if last_end is not None:
                    gaps += t - last_end
                start = t
            elif e == 1 and start is not None:
                busy += t - start
                n += 1
                last_end = t
                start = None
        if n:
            print(f"channel {ch}: {n} segments, busy {busy} cycles ({100 * busy // span}%), "
                  f"idle between segments {gaps} ({gaps // max(n - 1, 1)} per gap)")


if __name__ == "__main__":
    main(sys.argv[1])
