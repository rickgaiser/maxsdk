# maxsdk

A ground-up SDK for the PlayStation 2 Emotion Engine, built for the clang/LLVM R5900 toolchain. No
ps2sdk: below it are only the PS2 BIOS and the hardware. The plan is `../PLAN_MAXSDK.md`; measured
hardware facts are in [docs/](docs/README.md).

```
make                 # libmx.a, crt0.o, tests/*.elf, probe/*.elf (clang, ps2max build/libs/maxsdk)
make picolibc        # picolibc 1.8.9 (checkout + libc/picolibc-r5900.patch) for the R5900
make libc            # libmxc.a, librt.a and the C/C++ tests
make test-host       # host tests: generated fields, packet bytes
make check-deps      # fails if anything references ps2sdk
make gen             # regenerate include/mx/hw/*.h, include/mx/bios.h, src/base/syscalls.S
```

VU programs are HLSL (`examples/*/*.hlsl`, `tests/ps2/*.hlsl`, [docs/hlsl.md](docs/hlsl.md)).
A program on maxsdk includes `mk/maxsdk.mk`: the compiler flags, the link, and the rules for its
`.vsm` and `.hlsl` programs (`ps2max/apps/model/Makefile` is an example).

Run a test on PCSX2 with the `ps2-pcsx2-test` skill, on the PS2 with the `ps2-hw-test` skill
(it takes the PS2 lock and waits for the restart after the run). Every test ends with
`@@PS2TEST_END@@ status=N`.
A CPU exception ends the program with a report (registers, FPU, stack) and status 255;
`tools/symbolize.py <elf> <log>` turns its `trace:` line into a backtrace with functions and
source lines (build with `EXTRA_CFLAGS=-g` for the lines). Without frame pointers, the frames
after `ra` are return addresses found on the stack by a heuristic, so one can be stale.
`tests/ps2/crash.c` crashes on purpose to show it, so it always ends with status 255: scripts
that run every test and expect status 0 must skip it.
`make batch` builds `batch/ps2batch.elf`: all PS2 tests, the phase 2–5 probes and the
examples spin, scene and allunits in one ELF, so one PS2 run covers them.

New to maxsdk? Start with the [tutorials](tutorials/README.md): the basics (the frame loop,
GIF packets, the pad), graphics (triangles, textures, blending, the Z buffer), and VU1 programs
in HLSL, step by step with a program and a picture for each step.

Each example has its own directory in `examples/`:

| Example | |
|---|---|
| `spin`, `scene`, `allunits` | phase 3, 6, 7 |
| `vumax` | the VU1 vertex rate at the floor for six standard vertex programs (flat, Gouraud, textured, one light, lit + textured, ps2gl's SCEI): hand-scheduled kernels (`tools/vusched/vusched.py`) against the EE for every batch size, then throughput with and without the GS, next to the same programs in HLSL ([docs/vu1.md](docs/vu1.md)). Options: `s<k>` one scenario, `t` throughput only, `k` kernels alone, `c` plain DMA chain |
| `display` | every video mode (NTSC/PAL interlaced and progressive, 480p, 576p, 720p, 1080i, VESA) with the picture's edge marked, and the two read circuits merged: an overlay over 2x magnified animation, a crossfade, a fade to black, the smooth filter. Pad: left/right mode, square scene, d-pad/L1/R1 move the picture; without a press it shows each once and exits |
| `gsmax` | the GS at the rates of the GS manual's performance tables (setup per primitive type, fill rate), over PATH1 ([docs/gif-gs.md](docs/gif-gs.md)) |

Apps on maxsdk live in their own repos: `ps2max/apps/model` (a glTF model, skinned and animated,
drawn by HLSL shaders, supersampled), `ps2max/apps/procgen` (generated terrain), with the MXM
model converter in `ps2max/libs/mxm`.

Floating point: build with `-ffinite-math-only` (the Makefile does). The R5900 FPU has no INF/NaN,
so clang only uses it for finite math, and otherwise calls soft-float helpers that libmx does not
have.

## Status (2026-10-23)

Phases 0–8 (`../PLAN_MAXSDK.md`, `../PLAN_MAXSDK_NEXT.md`) are built and pass on PCSX2 and
the PS2; the measurements are in `docs/`. maxsdk is built with clang only.

| Phase | What | Tests |
|---|---|---|
| 0 | repo, generator (`gen/`), host tests, `docs/` | `make test-host` |
| 1 | crt0, linker script, BIOS syscall stubs, mini C runtime, printf, SIF tty client | `tests/ps2/hello.c` |
| 2 | interrupt dispatcher, DMA in every mode, MFIFO, stall control, interrupt-refilled queues + fences, timers, trace | `tests/ps2/dma.c`, `probe/p2_dma.c` |
| 3 | DMA/VIF/GIF packet builder, GS setup, FINISH/SIGNAL/vsync, frame loop, upload, readback | `tests/ps2/gs.c`, `tests/ps2/vif.c`, `probe/p3_gs.c`, `examples/spin/spin.c` |
| 4 | VU reset/control, COP2 transfers, microcode residency, kernels + queues, double buffering, T-bit, DMA readback | `tests/ps2/vu.c`, `probe/p4_vu.c`, `apps/llvm_bench/vudemo` |
| 5 | cursor emitters (`<mx/emit.h>`), MPG by ref tags, polled MARK waits, VU0 DMA readback, unit-state sampling, kernel launch parameters | `tests/host/test_emit.c`, `tests/ps2/vu.c`, `probe/p5_fast.c` |
| 6 | render pipeline (`<mx/frame.h>`): MFIFO-fed VIF1 stream, VU1 render kernel with double buffering + XGKICK, PATH3 textures, GS register cache, vblank accounting, VU1 compute between passes | `tests/ps2/frame.c`, `examples/scene/scene.c`, `examples/spin/spin.c` |
| 7 | `mx_sched` (`<mx/sched.h>`): pollers, pools, VU workers; all units at once | `examples/allunits/allunits.c`, `apps/llvm_bench/vudemo` (sched) |
| 8 | threads and semaphores on fences (`<mx/thread.h>`), picolibc + glue (`libc/`), compiler-rt builtins, a minimal C++ runtime, ps2sdk compat headers for bench (`compat/`) | `tests/ps2/thread.c`, `tests/libc/libc.c`, `tests/libc/cxx.cc`, `bench` (`SDK=maxsdk`) |

## Layout

| Path | |
|---|---|
| `include/mx/` | public headers; `hw/` is generated from `gen/hw_desc.py` |
| `src/base/` | syscall stubs, startup, mini C runtime, printf, SIF client (tty, files, IOP modules), cache ops; IOP memory over SIF and the patches of the ROM modules, after ps2sdk's sbv (`<mx/iop.h>`, `iop.c`) |
| `src/irq/`, `src/dma/` | interrupt dispatcher, DMA channels and the DMA engine |
| `src/pkt/`, `src/gs/` | DMA/VIF/GIF packets, chains (`<mx/chain.h>`: built once, sent or called many times), GS setup and interrupts, the VIF packets of HLSL programs and their mesh chains (`shader.c`) |
| `src/vu/` | VU control, kernels and streaming |
| `src/gs/video.c`, `rt.c`, `display.c` | video modes (`<mx/video.h>`), render targets in either GS context, also read as textures (`<mx/rt.h>`), what the two read circuits show and how they merge, per frame (`<mx/display.h>`) |
| `src/gs/frame3.c`, `src/vu/vu0r.c`, `vu0scei.vsm` | a second frame stream of GS packets over PATH3 next to VU1's (`mx_frame3_*` in `<mx/frame.h>`, GS context 2); the SCEI renderer on the EE and VU0 fed through VF registers into it (`<mx/vu0r.h>`) |
| `src/gs/hud.c`, `src/vu/render.vsm` | on-screen text and boxes (`<mx/hud.h>`); the VU1 render kernel of the frame pipeline (`render_vu`) |
| `src/gs/tex.c` | textures with MIPMAP levels (`<mx/tex.h>`): the level layout, TEX0/TEX1/MIPTBP of either context, uploads, K from the texel density, levels made by the GS for render targets |
| `src/gs/frame.c`, `src/sched/`, `src/thread/` | the render pipeline, the scheduler, threads |
| `libc/`, `compat/ps2sdk/` | picolibc glue, C++ runtime, atomics; the ps2sdk headers bench needs |
| `crt/`, `ld/` | crt0 and the linker script |
| `tests/host/`, `tests/ps2/` | host tests, PCSX2/PS2 tests |
| `probe/` | one ELF per hardware question; results go to `docs/` |
| `examples/<name>/` | the examples, one directory each |
| `mk/maxsdk.mk` | what a program on maxsdk includes: flags, link, the VU and HLSL rules |
| `tools/` | `symbolize.py` (exception backtraces), `trace.py`, `vusched/` (hand-scheduled VU1 kernels), `vscode-vsm/` |

## Decisions (PLAN_MAXSDK.md section 7)

| | Decision |
|---|---|
| D0 | Name `maxsdk`, prefix `mx_`, headers `<mx/…>`; own repo in `generic/llvm/maxsdk/` |
| D1 | C11, static inline headers + small .c files; assembly only in crt0, stubs and interrupt entry |
| D2 | One description file (`gen/hw_desc.py`) → generated headers, syscall stubs and host tests (committed) |
| D3 | Interrupts through BIOS dispatch with one SDK dispatcher per cause; polling in hot loops; own level-1 handler only if the measured gap is too large |
| D4 | Fence counters bumped by the interrupt handlers; an event is a (fence, value) pair |
| D5 | Packet builder with checks for general use, inline cursor emitters for hot loops, constant packets for static chains |
| D6 | Per-batch packets in SPR or uncached memory, static chains cached and written back once |
| D7 | Makefiles, clang only (gcc support dropped 2026-10-23) |
| D8 | Freestanding core (own mem*/str* (weak), printf, 64-bit divide helpers; no libc); picolibc as an option (phase 8) |
| D9 | Debug output over a minimal EE-side SIF tty client to the IOP file server (ps2link); EE SIO as fallback |
| D11 | DMA kept running by interrupt-refilled segment queues (all channels) + MFIFO for VIF1/GIF |
| D12 | GS: FINISH per frame/pass, SIGNAL for mid-frame release, LABEL for polled progress |
