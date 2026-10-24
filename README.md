# maxsdk

A ground-up SDK for the PlayStation 2 Emotion Engine, built for the clang/LLVM R5900 toolchain. No
ps2sdk: below it are only the PS2 BIOS and the hardware. The plan is `../PLAN_MAXSDK.md`; measured
hardware facts are in [docs/](docs/README.md).

```
make                 # libmx.a, crt0.o, tests/*.elf, probe/*.elf (clang, toolchain/build/maxsdk/clang)
make picolibc        # picolibc 1.8.9 (checkout + libc/picolibc-r5900.patch) for the R5900
make libc            # libmxc.a, librt.a and the C/C++ tests
make test-host       # host tests: generated fields, packet bytes
make check-deps      # fails if anything references ps2sdk
make gen             # regenerate include/mx/hw/*.h, include/mx/bios.h, src/base/syscalls.S
```

VU programs are HLSL (`demos/*.hlsl`, `tests/ps2/*.hlsl`, [docs/hlsl.md](docs/hlsl.md)).

Run a test on PCSX2 with the `ps2-pcsx2-test` skill, on the PS2 with
`ps2client -h 192.168.1.10 execee host:<elf>`. Every test ends with `@@PS2TEST_END@@ status=N`.
`make batch` builds `batch/ps2batch.elf`: all PS2 tests, the phase 2–5 probes and the demos
(spin, scene, allunits) in one ELF, so one PS2 run covers them.

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
| 3 | DMA/VIF/GIF packet builder, GS setup, FINISH/SIGNAL/vsync, frame loop, upload, readback | `tests/ps2/gs.c`, `tests/ps2/vif.c`, `probe/p3_gs.c`, `demos/spin.c` |
| 4 | VU reset/control, COP2 transfers, microcode residency, kernels + queues, double buffering, T-bit, DMA readback | `tests/ps2/vu.c`, `probe/p4_vu.c`, `bench/vudemo` |
| 5 | cursor emitters (`<mx/emit.h>`), MPG by ref tags, polled MARK waits, VU0 DMA readback, unit-state sampling, vuk launch parameters | `tests/host/test_emit.c`, `tests/ps2/vu.c`, `probe/p5_fast.c` |
| 6 | render pipeline (`<mx/frame.h>`): MFIFO-fed VIF1 stream, VU1 render kernel with double buffering + XGKICK, PATH3 textures, GS register cache, vblank accounting, VU1 compute between passes | `tests/ps2/frame.c`, `demos/scene.c`, `demos/spin.c` |
| 7 | `mx_sched` (`<mx/sched.h>`): pollers, pools, VU workers; all units at once | `demos/allunits.c`, `bench/vudemo` (sched) |
| 8 | threads and semaphores on fences (`<mx/thread.h>`), picolibc + glue (`libc/`), compiler-rt builtins, a minimal C++ runtime, ps2sdk compat headers for bench (`compat/`) | `tests/ps2/thread.c`, `tests/libc/libc.c`, `tests/libc/cxx.cc`, `bench` (`SDK=maxsdk`) |

## Layout

| Path | |
|---|---|
| `include/mx/` | public headers; `hw/` is generated from `gen/hw_desc.py` |
| `src/base/` | syscall stubs, startup, mini C runtime, printf, SIF tty client, cache ops |
| `src/irq/`, `src/dma/` | interrupt dispatcher, DMA channels and the DMA engine |
| `src/pkt/`, `src/gs/` | DMA/VIF/GIF packets, GS setup and interrupts |
| `src/vu/` | VU control, kernels and streaming |
| `src/gs/frame.c`, `src/sched/`, `src/thread/` | the render pipeline, the scheduler, threads |
| `libc/`, `compat/ps2sdk/` | picolibc glue, C++ runtime, atomics; the ps2sdk headers bench needs |
| `crt/`, `ld/` | crt0 and the linker script |
| `tests/host/`, `tests/ps2/` | host tests, PCSX2/PS2 tests |
| `probe/` | one ELF per hardware question; results go to `docs/` |

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
