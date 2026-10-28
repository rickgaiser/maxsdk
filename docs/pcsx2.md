# Where PCSX2 differs from the PS2

Correctness notes only. PCSX2 (flatpak 2.5.x) timings are never performance data.

Screenshots (`host:*.tga`) and pictures read back from GS memory are taken with the software
renderer (`Renderer = 13` in `[EmuCore/GS]` of the flatpak's `inis/PCSX2.ini`, since
2026-10-28): the hardware renderers approximate GS features such as MIPMAP level selection.

| Behaviour | PS2 | PCSX2 | Source |
|---|---|---|---|
| Multiplier with a power of two in fs | 1 ULP low | exact in both orders | `bench/vudemo/mulprobe.c` |
| DMA tag with an uncached (0x2…) address | crash | masked, works | `bench/vudemo` |
| Mapped VU memory writes + CMSAR1/VCALLMS start | wrong VU results | works | `bench/vudemo` |
| VIF0 chain with tags written uncached | does not start | runs | `bench/vudemo` |
| Debug output | SIF tty → ps2link → ps2client | SIF tty → IOP console, EE SIO → EE console (both go to stdout with `-earlyconsolelog`) | maxsdk `tests/ps2/hello.c` |
| After `Exit` | back to ps2link | loads `rom0:OSDSYS`, fails, reboots the IOP | maxsdk `tests/ps2/hello.c` |
| Stall control with the drain (VIF1) in **normal** mode | stalled (manual: normal-mode drains are stall controlled) | not stalled: the drain read the buffer before the source wrote it | maxsdk `tests/ps2/dma.c` (first version) |
| Stall control with a `refs` packet on a chain drain | stalls; data correct (`tests/ps2/dma.c`) | stalls until the source has written | maxsdk `tests/ps2/dma.c` |
| DMA transfers in the engine test | take time; the queue fills | finish almost at once (each segment done before the next is submitted) | maxsdk `tests/ps2/dma.c` |
| fromSPR reading a source that the EE rewrites right after `mx_mfifo_push` | 53 of 192 words corrupted | no corruption (the DMA is done at once) | maxsdk `tests/ps2/dma.c` (first version) |
| VIF0 chain with tags written uncached (isolated probe) | starts, data ok | starts, data ok | maxsdk `probe/p2_dma.c` |
| Reading VU memory while the VU runs | wrong data | correct data | maxsdk `tests/ps2/vu.c` |
| VU1 D bit with FBRST.DE1 | hangs the PS2 | ignored | maxsdk `tests/ps2/dbit.c` |
| FBRST ForceBreak (VFS) | stops the VU in 36 cycles | ignored; a VCALLMS to the still running VU0 then freezes PCSX2 (kill it with `pkill -9 -f '^pcsx2-qt'`) | maxsdk vustate.c (2026-10-24, in `maxsdk_vu_work_2026-10-24/`) |
| VU1 running a spin loop / stopped by a T bit | VBS1 / VTS1 set | neither shows in VPU-STAT | maxsdk vustate.c (`maxsdk_vu_work_2026-10-24/`) |
| VIF1 FLUSHE behind a T-stopped VU1 | waits until a reset or resume | passes, MARK set | maxsdk vustop.c (`maxsdk_vu_work_2026-10-24/`) |
| CTC2 CMSAR0 directly followed by VCALLMSR | starts at the previous CMSAR0 | starts at the new one | maxsdk vustate.c (`maxsdk_vu_work_2026-10-24/`) |
| D bit on VU0, VU0 watchdog | hangs the PS2; no watchdog in 5 s | ignored; no watchdog | maxsdk vudbit.c, vustate.c (`maxsdk_vu_work_2026-10-24/`) |
| spin demo, 120 frames | 146 vsyncs | 119 vsyncs | maxsdk `demos/spin/spin.c` |
| A second SIGNAL before CSR.SIGNAL is cleared (GS interrupts masked) | the same: the GS stops until CSR.SIGNAL is written | the GS stops at the second SIGNAL (SIGID 0x11, the LABEL after it not yet set) and goes on once CSR.SIGNAL is written | maxsdk `probe/p5_fast.c` |
| Stall control, normal-mode VIF1 drain (`probe/p5_fast.c` stall_normal) | the drain waits; data correct | the drain does not wait: 28 of 28 words wrong | maxsdk `probe/p5_fast.c` |
| Timing of everything in phases 5-7 (frame pipeline, mx_sched, sampling) | | DMA, VU and GS work finish almost at once: busy shares read 0 %, there are no missed vblanks | maxsdk `demos/scene/scene.c`, `demos/allunits/allunits.c` |

## PLAN_PROCGEN (PS2 and PCSX2 2.5, 2026-10-23/24)

| Behaviour | PS2 | PCSX2 | Source |
|---|---|---|---|
| VU FMAC multiply with 1.0 in **fs** (`MULw.w d, vf0, s`, the VU target's old field copy) | 1 ULP towards 0 for 4096 of 4096 random values (powers of two: exact) | exact | `probe/p10_vucopy.c` |
| VU FMAC multiply with 1.0 in **ft** (`MULw.w d, s, vf0w`), ADD with 0, MOVE, MR32, ACC moves, DIV by 1 | exact | exact | `probe/p10_vucopy.c` |
| VU add of a small operand to a large one (`(v + 1.5·2^23) − 1.5·2^23`, the magic-constant floor) | the smaller operand is cut to one bit below the sum's last first: floor is 1 too high for a negative v with a fraction below 0.5 (1040 of 8192 values) | exact | `tests/ps2/floor.c` |
| procgen's dust: the box wrap `q − floor(q)` of negative values | wrong (all motes out of view, none drawn) | right | `tests/ps2/dust.c` |
| fBm noise on VU0 against the EE, 6 octaves (before the two rows above were handled) | 1022 of 1089 samples agree | 66 of 1089 | `tests/ps2/noise.c` |
| The same with exact copies (LLVM) and the exact floor (vuk) | 1089 of 1089 | 1089 of 1089 | `tests/ps2/noise.c` |
| A load through a bad pointer (procgen, address 0x1a8, from an LLVM miscompile) | TLB miss exception | reads low memory, the program goes on | `demos/procgen/procgen.c`, exception report |
| A load from address 0x2 (misaligned) | not measured | no exception, returns 0 | scratch test of `src/base/exception.c` |
| GS SPRITE with UV at texel edges, 1:1 (the font) | the last column and row of each glyph are not drawn; UV + 8 (half a texel) draws them | drawn either way | procgen screenshots |
| Debug output of a line written in several writes, or the first lines when the SIF is first used in the middle of GS/DMA set-up | pieces lost (ps2link's udptty: one UDP broadcast per write) | nothing lost | `demos/procgen/levels.c`, `demos/procgen/procgen.c` |
| procgen max count (no vsync) | 192 fps, 20.5 M vertices/s, VU1 99 % | 290 fps; busy shares 0 % | `demos/procgen/procgen.c` (PCSX2 timings are not data) |

## The PATH3 stream (PCSX2 2.5 and the PS2, 2026-10-28)

| Behaviour | PS2 | PCSX2 | Source |
|---|---|---|---|
| VIF1 `MARK` with the I-bit | not measured (`mx_frame_present` relies on it stopping) | the interrupt comes (once VIF1 has been let go on before, VIF1_STAT.INT), VIF1 does not stop; VIF1_CODE reads 0 in the handler. An I-bit `NOP` after the `MARK` stops it, and VIF1_MARK has the value | `src/gs/frame3.c`, `tests/ps2/frame3.c` |
| VIF1_STAT at the interrupt of an I-bit code; STC written then | STC between INT and VIS does **not** keep VIF1 from stopping: it stopped at the join for good (no further interrupt). Wait for VIS, then STC | INT set, VIS not yet (it follows when VIF1 tries the next code); STC clears both and VIF1 goes on | `src/gs/frame3.c` (`release`), `demos/model -vu0` |
| `ctc2` CMSAR0 right before `vcallmsr` | starts at the **previous** CMSAR0 (already in [vu0.md](vu0.md)): the VU0 renderer's two register sets ran each other's programs, a different picture each run | starts at the new one | `src/vu/vu0r.c`, `demos/model -vu0` |
| A reservation larger than a frame half (`mx_frame_reserve`) | ran into the next slot of the VIF1 stream while it was queued: VIF1 read colour data as VIFcodes (VIF1_CODE `ff595378`) and stopped | each slot is sent before the next is filled: no harm seen | `demos/common/hud.h` (now sized by `mx_frame_room`); `mx_frame_reserve` now ends the program |
| GIF_TAG0..P3TAG | not measured | read 0 | debugging `tests/ps2/frame3.c` |
| A PATH3 GIF packet with fewer quadwords than its tag says | not measured | PATH3 keeps the GIF (GIF_STAT APATH 3, OPH 1, FQC 0) after the DMA ends; VIF1's next DIRECT or FLUSHA waits for ever | a test bug in `tests/ps2/frame3.c` |

## Chains (PS2 and PCSX2 2.5, 2026-10-27)

| Behaviour | PS2 | PCSX2 | Source |
|---|---|---|---|
| MFIFO drain (VIF1, TTE) reads a `call` tag to a chain outside the ring | not followed: the drain stops (CHCR.STR 0, D_STAT.CIS set, CHCR.ASP reads 1); nothing after the call is sent | not followed: the drain stops (CHCR.STR 0, D_STAT.CIS set, CHCR.ASP reads 2); the chain and what follows the call in the ring are not sent | `probe/p18_mfifo_call.c` |
| The same with a `next` tag | not followed: the drain stops (STR 0, CIS set, ASP 0, TADR on the tag) | not followed: the drain holds at the tag (STR 1, TADR on it) | `probe/p18_mfifo_call.c` |
| The same with a `ref` tag (the control) | followed, the ring drains | followed, the ring drains | `probe/p18_mfifo_call.c` |
