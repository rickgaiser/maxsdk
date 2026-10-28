# maxsdk hardware notes

Everything about the PS2 Emotion Engine that we **measured**, or that is **missing from (or wrong in)
the Sony manuals** (`ps2max/docs/md`), in one place. These notes are the basis for every performance
decision in maxsdk and in the compiler.

| File | Content |
|---|---|
| [measuring.md](measuring.md) | How to measure: counters, probes, repetitions, cache state |
| [ee-core.md](ee-core.md) | R5900 pipelines, latencies, pairing, hazards |
| [fpu.md](fpu.md) | FPU deviations from IEEE 754, multiplier behaviour |
| [memory.md](memory.md) | Caches, uncached/UCAB/SPR, cache operation costs |
| [interrupts.md](interrupts.md) | Interrupt latency and BIOS dispatch costs |
| [dma.md](dma.md) | DMAC: addresses, throughput, chain restart gaps, MFIFO |
| [vif.md](vif.md) | VIF codes: costs and behaviour |
| [gif-gs.md](gif-gs.md) | GIF paths, GS SIGNAL/FINISH/LABEL, vsync |
| [vu0.md](vu0.md), [vu1.md](vu1.md) | VU macro/micro mode, transfers, control |
| [hlsl.md](hlsl.md) | Writing VU programs in HLSL (not measurements: how the toolchain is used) |
| [bios.md](bios.md) | EE kernel (BIOS) behaviour the SDK relies on |
| [sync.md](sync.md) | Patterns that keep all units busy, and their idle gaps |
| [pcsx2.md](pcsx2.md) | Where PCSX2 differs from the PS2 (correctness only) |

## Rules

* One fact per entry: **fact**, **value**, **conditions** (cache state, alignment, channel, sizes),
  **source** (probe file + commit), **date**, and **manual** (what the Sony docs say, if different).
* Only measured values or facts missing from the manuals. A fact the manual states correctly gets at
  most one line with a reference.
* **PS2 measurements only.** PCSX2 results go to `pcsx2.md`, and only as correctness notes. PCSX2
  timings are never performance data.
* Every number can be measured again: it names its probe. New probes live in `maxsdk/probe/`. Facts
  imported in phase 0 name the bench file they came from (`bench/` is the `test_llvm_01` repo).

## Hardware used

| | |
|---|---|
| PS2 | the test PS2 at 192.168.1.10, started by ps2link, programs run with `ps2client execee` |
| BIOS | ROMVER `0200EC20040614`: 2.00, Europe, consumer, 2004-06-14 (a PAL slim); `probe/bios_info.c`, 2026-10-27 |
| Clock | EE 294.912 MHz (COP0 Count runs at this rate) |
