# EE core (R5900)

Measured with the kernels in `apps/llvm_bench/uarch_tests.c` (bench master, runs of 2026-10-20 and 2026-10-21)
on the PS2. The LLVM scheduling model `MipsScheduleR5900.td` and the issue model
`MipsR5900IssueModel` are fitted to these numbers. Latency = cycles until a dependent instruction
can issue; throughput = cycles between independent ones.

## Integer and MMI

| Fact | Value | Conditions / notes |
|---|---|---|
| Simple 128-bit MMI ops (paddw, pcgtw, pmaxw, pand, psllw, pextlw, ppacw, pcpyld, pcpyud, pexew, pcpyh, prot3w, plzcw, psubw, por, pxor, pnor, paddb/h, pceqw, pminw/h, paddsw, paddub, psubuh/uw, pabsw/h, psrlw, psraw, psrah, psllh) | latency 1, 1/cycle | |
| pmultw, pmulth, 3-operand mult, mult1 → rd | latency 4, throughput 2 | pmflo +1 |
| mult and mult1 | run in parallel | |
| madd/madd1/pmaddw ACC → ACC | 2 | rd at 4; mtlo → madd 1 |
| pmaddh ACC → ACC | 2 | rd 4, pmaddh → pmflo 4, phmadh rd 4; pairs with ldl and pcpyld |
| mult1 + mflo1 | 5 | pmultw + pmfhl.lw 5 |
| div / div1 | 37 (+mflo 1), throughput 37 | ALU not blocked; div \| div1 in parallel; div1 \| mult1 39 (MAC1 busy) |
| qfsrv | latency 1, wide | no ALU pair, pairs with lw; mtsab → qfsrv 1 |
| ldl → ldr merge | 1 | |
| lq | latency 2 | lq \| lq and sq \| sq do not pair (one load/store unit) |
| A 128-bit MMI op does **not** pair with an ALU op, plzcw or mult1 | 2 cycles per pair | **Manual** says a 1-cycle "Y" stall; measured worse. Pairs with lw/lq/sq. |
| 64 ↔ 128-bit forwarding | no extra latency | |
| Store → load to the same address | no penalty | sw→lw, sq→lw, sw→lq all 3 = 1 + 2; 4× sw → lq 6 |
| Unaligned 128-bit load | 6.25 with ldl/ldr ×2 + pcpyld | via the stack (ldl/ldr ×2, sd ×2, lq) 11.2 |
| Unaligned 128-bit store | 5.1 with pcpyud + sdl/sdr ×2 | via the stack 7.1 |
| pmflo and pabsw | do not pair with addu | pmfhi / psrlw pair with lw |

## MULT/MULT1 write-port rule

After a 3-operand MULT (pipe 0) or MULT1 (pipe 1) with rd ≠ $0, a GPR-writing instruction **in the
same pipe exactly 2 cycles later** stalls 1 cycle (+1, +3, +4, +5 are free; rd = $0 never stalls; sw
never). Measured with clean fillers (add.s | sw), 2026-10-21. MULT1/MADD1/DIV1/MFHI1/MFLO1 issue in
pipe 1: they do not pair with lw/sw or each other, but pair with ALU ops.

## FPU timing

| Fact | Value |
|---|---|
| max.s / min.s | 4 |
| rsqrt.s | 13 (sqrt.s 7 + div.s 7 = 14) |
| c.eq.s → bc1f/bc1t | 6 |
| c.eq.s → bc1t, mfc1 → use, mtc1 → FPU use | interlocked, no nop needed (MIPS III delay-slot nops are waste) |

## Branches

| Fact | Value |
|---|---|
| branch + nop | 1 cycle, taken: no penalty |
| mispredict | 3 cycles (random 9.75 vs predictable 8.25 per iteration) |

## Not measured yet

maddu, msub, pmsubw, pmaddUw, the H variants (modeled like madd/pmaddw), SA pairing.
