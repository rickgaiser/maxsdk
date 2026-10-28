# EE kernel (BIOS)

What maxsdk relies on, with how it was checked.

| Fact | Source | Date |
|---|---|---|
| `SetupThread` (syscall 0x3c): a0 = gp, a1 = stack (-1 = top of memory), a2 = stack size, a3 = argument block the kernel fills (argc, argv[16], 256 bytes), t0 = root function; returns the stack pointer | `tests/ps2/hello.c` on PCSX2 and the PS2 (argc = 1, argv[0] = `host:hello.elf`) | 2026-10-23 |
| `SetupHeap(_end, -1)` returns the end of the heap: 0x01fdf000 with a 128 KiB stack | `tests/ps2/hello.c`, PCSX2 | 2026-10-23 |
| Started by ps2link, the IOP side of SIF is set up: `SIF_SYSREG_SUBADDR` and `SIF_SYSREG_RPCINIT` are set, so a new EE program only sends CHANGE_SADDR with its receive buffer. The file server (RPC 0x80000001) answers writes to fd 1, which ps2link sends to ps2client. | `src/base/sif.c`, `tests/ps2/hello.c` on the PS2 | 2026-10-23 |
| Under PCSX2 `-elf` the same SIF path works (the IOP console shows the tty output) | `tests/ps2/hello.c` on PCSX2 | 2026-10-23 |
| SIF set up and the file server bound at the first output, in the middle of a program's GS/DMA/interrupt set-up (`apps/procgen/levels.c`): lines were lost on the PS2 (the first, and later ones); ps2link's tty is udptty, one UDP broadcast per write, whose errors nobody sees. Bound in the startup instead (`mx_debug_init`): nothing lost | `apps/procgen/levels.c` on the PS2 | 2026-10-23 |
| LOADFILE (RPC 0x80000006, function 0: path, args) loads `rom0:SIO2MAN` and `rom0:PADMAN` under ps2link and PCSX2; PADMAN (RPC 0x8000010F/0x8000011F, the ROM protocol) writes two 64-byte records per port into EE memory every frame; a pad is read 0.2 s after the open | `tests/ps2/pad.c` on the PS2 and PCSX2 | 2026-10-23 |
| SetVTLBRefillHandler (13) / SetVCommonHandler (14) for codes 1–3, 4–7, 9, 10, 12, 13: the kernel jumps to the handler with only k0/k1 changed; an ERET into a report function runs it with interrupts and the SIF working (`src/base/exception.S`) | a TLB miss (load) in procgen reported with all registers | `apps/procgen/procgen.c` on the PS2; a `break` on PCSX2 | 2026-10-23 |
| PCSX2 does not raise address errors (a load from 0x2 returned 0) nor TLB misses at 0x1a8 (it read kernel memory): a wrong pointer that crashes the PS2 runs on PCSX2 | | PCSX2 2.5 | 2026-10-23 |
| Interrupt handlers (AddIntcHandler/AddDmacHandler) run with interrupts disabled, with the gp of the registering code; GPRs are saved, FPU registers are not; returning -1 skips later handlers | EE Kernel Reference, AddIntcHandler | manual |
| `MachineType` returns 0 and `GetMemorySize` 33554432 on the test PS2 | `probe/p2_dma.c` | 2026-10-23 |
| `Exit` returns to ps2link: three ELFs ran back to back with `ps2client execee` without a reboot | PS2 session | 2026-10-23 |
| File server READ (RPC 0x80000001, function 2: fd, ptr, size, record): the IOP sends the 16-byte aligned middle by SIF DMA to ptr and the parts before and after it (up to 15 bytes each, with their addresses) in the 48-byte record; the parts are those of the size asked for, so after a short read (the end of the file) the DMA ends on the quadword after the last byte read and the part after it is not data. LSEEK is function 4 (fd, offset, whence) | `tests/ps2/fio.c` on PCSX2 and the PS2 | 2026-10-24 |
| ps2link (host: by ps2client) takes a negative offset of LSEEK from the current position or the end as a huge positive one: it answers with the right position (modulo 2^32), but its file is then past the end and reads give 0 bytes; PCSX2 does not. `mx_fio_lseek` seeks from the position itself. CLOSE answers with the descriptor on ps2link, 0 on PCSX2 | `tests/ps2/fio.c` on the PS2 and PCSX2 | 2026-10-24 |
