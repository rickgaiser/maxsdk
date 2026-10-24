# maxsdk: build the library, tests and probes with clang (the LLVM R5900
# toolchain in this tree; gcc is not supported).
#
#   make [OUT=<dir>]                        library, test and probe ELFs
#   make test-host                          host tests (generated fields, packets)
#   make check-deps                         fail if anything links ps2sdk
#   make gen                                regenerate the hardware headers
#
# Output goes to toolchain/build/maxsdk/clang/ by default.

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   := $(abspath $(HERE)/../../../..)
OUT      ?= $(PS2MAX)/toolchain/build/maxsdk/clang
LLVM     ?= $(PS2MAX)/toolchain/build/llvm/bin

HOSTCC   ?= cc

# -ffinite-math-only: the R5900 FPU has no INF/NaN, so clang only uses it for
# finite math; without the flag it calls soft-float helpers (not in libmx).
# -fno-math-errno: the math functions do not set errno, so sqrtf is sqrt.s.
COMMON_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding \
                -fno-common -ffinite-math-only -fno-math-errno -I$(HERE)/include -G0
CC       = $(LLVM)/clang
CFLAGS   = $(COMMON_CFLAGS) --target=mips64el -mcpu=r5900 -mabi=n32 -fno-pic -mno-abicalls \
           -fomit-frame-pointer
# $(call link,out,objs)
link     = $(LLVM)/ld.lld -m elf32ltsmipn32 -T$(HERE)/ld/maxsdk.ld -z max-page-size=128 \
           --gc-sections -Map $(1:.elf=.map) -o $(1) $(2)
CFLAGS  += $(EXTRA_CFLAGS)

LIB_SRCS := $(wildcard $(HERE)/src/*/*.c) $(wildcard $(HERE)/src/*/*.S)
LIB_OBJS := $(patsubst $(HERE)/%,$(OUT)/obj/%.o,$(LIB_SRCS))
LIB       = $(OUT)/libmx.a
CRT0      = $(OUT)/obj/crt0.o
HDRS     := $(wildcard $(HERE)/include/mx/*.h $(HERE)/include/mx/hw/*.h)

# Each demo has a directory in demos/ (demos/common: VU code that several
# use); program and VU program names are unique over all of them.
DEMO_DIRS := $(patsubst %/,%,$(wildcard $(HERE)/demos/*/))

# VU microcode (*.vsm, assembled by llvm-mc -triple=vu): each becomes an EE
# object with the code between the symbols <name>_vu and <name>_vu_end.
VSMS     := $(wildcard $(HERE)/tests/ps2/*.vsm $(HERE)/probe/*.vsm $(HERE)/demos/*/*.vsm)
VU_OBJS  := $(patsubst %.vsm,$(OUT)/vu/%.o,$(notdir $(VSMS)))
vpath %.vsm $(HERE)/tests/ps2 $(HERE)/probe $(DEMO_DIRS)

$(OUT)/vu/%.vu.elf: %.vsm
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-mc -triple=vu -mcpu=vu1 -filetype=obj $< -o $@

$(OUT)/vu/%.bin: $(OUT)/vu/%.vu.elf
	$(LLVM)/llvm-objcopy -O binary --only-section=.vutext $< $@

$(OUT)/vu/%.o: $(OUT)/vu/%.bin
	printf '%s\n' '.section .vutext, "a"' '.balign 16' '.globl $*_vu' '$*_vu:' \
	  '.incbin "$<"' '.globl $*_vu_end' '$*_vu_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@

# HLSL programs (include/vu.hlsli): clang's HLSL for SPIR-V, then opt and
# llc for the VU (VUHLSLAdapter, VUEntryLowering); the layout header <name>.h
# from section .vu.header, wrapped like the microcode above. HLSL_VU0 lists
# the programs for VU0; the others run on VU1. (-Wno-deprecated-builtins:
# clang's own HLSL headers use them for max and min.)
#
# HLSL_IX: vertex programs that are also built indexed (docs/hlsl.md), from
# the same <name>.hlsl as program <name>_ix: IX_N vertices per batch (or
# IX_N_<name>), the header's macros VU_IX_<NAME>_..., MX_VU_SHADER(IX_<NAME>,
# <name>_ix_vu, <name>_ix_vu_end).
HLSLS     := $(wildcard $(HERE)/tests/ps2/*.hlsl $(HERE)/demos/*/*.hlsl)
HLSL_IX   := lvl0 lvl1 lvl2 mflat mlit mtex mskin
IX_N      := 32
HLSL_NAMES = $(basename $(notdir $(HLSLS))) $(addsuffix _ix,$(HLSL_IX))
HLSL_OBJS := $(patsubst %,$(OUT)/hlsl/%.o,$(HLSL_NAMES))
HLSL_HDRS := $(patsubst %,$(OUT)/hlsl/%.h,$(HLSL_NAMES))
HLSL_VU0  := fbm dust floor nest
vpath %.hlsl $(HERE)/tests/ps2 $(DEMO_DIRS)
CFLAGS   += -I$(OUT)/hlsl
VU_CPU    = $(if $(filter $*,$(HLSL_VU0)),vu0,vu1)

$(OUT)/hlsl/%.spv.ll: %.hlsl $(HERE)/include/vu.hlsli $(LLVM)/clang
	@mkdir -p $(dir $@)
	$(LLVM)/clang -cc1 -triple spirv-unknown-vulkan1.3-library -x hlsl -emit-llvm -O2 \
	  -finclude-default-header -I$(HERE)/include -Wno-deprecated-builtins $< -o $@

$(OUT)/hlsl/%.vu.ll: $(OUT)/hlsl/%.spv.ll $(LLVM)/opt
	$(LLVM)/opt -mtriple=vu -O2 -vu-cpu=$(VU_CPU) $< -S -o $@

# the indexed program from the same SPIR-V (the shorter stem wins over the
# rule above)
$(OUT)/hlsl/%_ix.vu.ll: $(OUT)/hlsl/%.spv.ll $(LLVM)/opt
	$(LLVM)/opt -mtriple=vu -O2 -vu-cpu=vu1 -vu-indexed=$(or $(IX_N_$*),$(IX_N)) \
	  -vu-header-prefix=VU_IX_ $< -S -o $@

$(OUT)/hlsl/%.vu.elf: $(OUT)/hlsl/%.vu.ll $(LLVM)/llc
	$(LLVM)/llc -mtriple=vu -mcpu=$(VU_CPU) -O3 -filetype=obj $< -o $@

$(OUT)/hlsl/%.h: $(OUT)/hlsl/%.vu.elf
	$(LLVM)/llvm-objcopy --dump-section .vu.header=$@.tmp $< /dev/null
	{ echo '/* generated from $*.hlsl */'; echo '#ifndef $(shell echo $* | tr a-z A-Z)_H'; \
	  echo '#define $(shell echo $* | tr a-z A-Z)_H'; cat $@.tmp; echo '#endif'; } > $@
	rm -f $@.tmp

$(OUT)/hlsl/%.bin: $(OUT)/hlsl/%.vu.elf
	$(LLVM)/llvm-objcopy -O binary --only-section=.vutext $< $@

$(OUT)/hlsl/%.o: $(OUT)/hlsl/%.bin
	printf '%s\n' '.section .vutext, "a"' '.balign 16' '.globl $*_vu' '$*_vu:' \
	  '.incbin "$<"' '.globl $*_vu_end' '$*_vu_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@

TESTS    := $(patsubst $(HERE)/tests/ps2/%.c,$(OUT)/tests/%.elf,$(wildcard $(HERE)/tests/ps2/*.c))
PROBES   := $(patsubst $(HERE)/probe/%.c,$(OUT)/probe/%.elf,$(wildcard $(HERE)/probe/*.c))
DEMO_SRCS := $(wildcard $(HERE)/demos/*/*.c)
DEMOS    := $(patsubst %.c,$(OUT)/demos/%.elf,$(notdir $(DEMO_SRCS)))
vpath %.c $(DEMO_DIRS)

all: $(LIB) $(CRT0) $(TESTS) $(PROBES) $(DEMOS) $(OUT)/batch/ps2batch.elf

$(OUT)/obj/%.c.o: $(HERE)/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.S.o: $(HERE)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The mem*/str* bodies must not be turned into calls to themselves.
$(OUT)/obj/src/base/string.c.o: CFLAGS += -fno-builtin

$(CRT0): $(HERE)/crt/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

# Each test or probe is one C file, linked with crt0 and libmx only (libmx
# has the compiler helper functions; no libgcc).
$(OUT)/tests/%.elf: $(HERE)/tests/ps2/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

$(OUT)/probe/%.elf: $(HERE)/probe/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PROBE_CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

# VU0 macro mode (COP2 instructions in inline asm)
$(OUT)/probe/p10_vucopy.elf: PROBE_CFLAGS = -mvu0
$(OUT)/probe/p11_vu0mix.elf: PROBE_CFLAGS = -mvu0

# A demo's C file finds the headers of its directory.
$(OUT)/demos/%.elf: %.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) \
                    $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(dir $<) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

# ---- C library and C++ runtime (phase 8, D8c) -------------------------------
# picolibc (built by `make picolibc` into PICOLIBC) with maxsdk's glue
# (libc/: stdio on the debug output, sbrk, _exit, a minimal C++ runtime, and
# the ps2sdk calls bench uses). Programs that use libc link
#   crt0, objects, libmxc.a, libc.a, libmx.a, compiler-rt builtins
# (soft double and the float helpers that code without -ffinite-math-only
# calls). Skipped while picolibc is not built.

PICOLIBC     ?= $(PS2MAX)/toolchain/build/picolibc/install
PICOLIBC_SRC ?= $(PS2MAX)/toolchain/build/picolibc/src
# compiler-rt's generic builtins, built like libmx (non-PIC; the toolchain's
# own libclang_rt.builtins.a is PIC and its GOT relocations do not link here)
RT_DIR       ?= $(abspath $(HERE)/../llvm-project/compiler-rt/lib/builtins)
RT_SKIP       = divxc3 emutls enable_execute_stack eprintf extendhfxf2 fixunsxfdi fixunsxfsi \
                fixunsxfti fixxfdi fixxfti floatdixf floattixf floatundixf floatuntixf mulxc3 \
                powixf2 truncdfbf2 truncsfbf2 truncxfhf2 clear_cache trampoline_setup \
                os_version_check gcc_personality_v0 atomic
RT_SRCS      := $(filter-out $(patsubst %,$(RT_DIR)/%.c,$(RT_SKIP)),$(wildcard $(RT_DIR)/*.c))
RT_BUILTINS   = $(OUT)/librt.a
RT_CFLAGS     = -std=gnu11 -O2 -ffreestanding -fno-builtin -w -G0 --target=mips64el -mcpu=r5900 \
                -mabi=n32 -fno-pic -mno-abicalls
# hosted, not freestanding: libc is there, and C++ only treats main as the
# program's entry (unmangled) in a hosted build
LIBC_CFLAGS   = $(filter-out -ffreestanding,$(CFLAGS)) -isystem $(PICOLIBC)/include \
                -I$(HERE)/compat/ps2sdk
CXXFLAGS      = $(filter-out -std=gnu11,$(LIBC_CFLAGS)) -std=gnu++17 -fno-exceptions -fno-rtti
CXX           = $(LLVM)/clang++
LIBMXC        = $(OUT)/libmxc.a
# the glue is linked as an object, so its sbrk and stdio win over picolibc's
# whatever the archive order
LIBC_LIBS     = $(OUT)/libc/glue.c.o $(LIBMXC) $(PICOLIBC)/lib/libc.a $(LIB) $(RT_BUILTINS)
HAVE_LIBC    := $(wildcard $(PICOLIBC)/lib/libc.a)

$(OUT)/libc/%.c.o: $(HERE)/libc/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

$(OUT)/libc/%.cc.o: $(HERE)/libc/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/rt/%.o: $(RT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(RT_BUILTINS): $(patsubst $(RT_DIR)/%.c,$(OUT)/rt/%.o,$(RT_SRCS))
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

$(LIBMXC): $(OUT)/libc/glue.c.o $(OUT)/libc/compat.c.o $(OUT)/libc/atomic.c.o $(OUT)/libc/cxx.cc.o
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

LIBC_TESTS := $(patsubst $(HERE)/tests/libc/%,$(OUT)/libc/tests/%.elf,\
                $(basename $(wildcard $(HERE)/tests/libc/*.c $(HERE)/tests/libc/*.cc)))

$(OUT)/libc/tests/%.elf: $(HERE)/tests/libc/%.c $(LIBMXC) $(LIB) $(CRT0) $(RT_BUILTINS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(LIBC_LIBS))

$(OUT)/libc/tests/%.elf: $(HERE)/tests/libc/%.cc $(LIBMXC) $(LIB) $(CRT0) $(RT_BUILTINS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(LIBC_LIBS))

ifneq ($(HAVE_LIBC),)
all: $(LIBMXC) $(RT_BUILTINS) $(LIBC_TESTS)
endif
libc: $(LIBMXC) $(RT_BUILTINS) $(LIBC_TESTS)

# Build picolibc from PICOLIBC_SRC (a picolibc 1.8.9 checkout with
# libc/picolibc-r5900.patch applied) for the R5900, n32, with clang.
picolibc:
	@mkdir -p $(PICOLIBC)/../build-meson
	sed "s|@LLVM@|$(LLVM)|g" $(HERE)/libc/cross-r5900.txt.in > $(PICOLIBC)/../cross.txt
	meson setup --wipe $(PICOLIBC)/../build-meson $(PICOLIBC_SRC) --cross-file $(PICOLIBC)/../cross.txt \
	  --prefix=$(PICOLIBC) -Dmultilib=false -Dpicocrt=false -Dsemihost=false -Dtests=false \
	  -Dthread-local-storage=false -Dposix-io=false -Dspecsdir=none -Dio-long-long=true \
	  -Dbuildtype=release
	ninja -C $(PICOLIBC)/../build-meson install

# ---- the combined PS2 batch ---------------------------------------------------
# One ELF that runs the tests, probes and demos below in a row, so a single
# PS2 run covers them. Each program's main() is renamed to <name>_main.

BATCH_SRCS := tests/ps2/dma.c tests/ps2/gs.c tests/ps2/vif.c tests/ps2/vu.c \
              tests/ps2/frame.c probe/p2_dma.c probe/p3_gs.c probe/p4_vu.c probe/p5_fast.c \
              demos/spin/spin.c demos/scene/scene.c demos/allunits/allunits.c tests/ps2/thread.c \
              tests/ps2/dbit.c
BATCH_OBJS := $(patsubst %.c,$(OUT)/batch/%.o,$(BATCH_SRCS))

$(OUT)/batch/%.o: $(HERE)/%.c $(HDRS) $(HLSL_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMX_BATCH -Dmain=$(notdir $*)_main -I$(HERE)/tests/ps2 -c $< -o $@

$(OUT)/batch/ps2batch.elf: $(HERE)/tests/batch/ps2batch.c $(BATCH_OBJS) $(LIB) $(CRT0) $(VU_OBJS) \
                           $(HLSL_OBJS) $(HERE)/ld/maxsdk.ld
	$(CC) $(CFLAGS) -DMX_BATCH -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(BATCH_OBJS) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

batch: $(OUT)/batch/ps2batch.elf

# ---- host tests -----------------------------------------------------------

# Pure parts of the library are tested on the host too.
HOST_SRCS := $(wildcard $(HERE)/tests/host/*.c) $(HERE)/src/pkt/pkt.c
HOST_BIN   = $(OUT)/host/test_host

$(HOST_BIN): $(HOST_SRCS) $(wildcard $(HERE)/tests/host/*.h) $(HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -Wall -Wextra -DMX_HOST -I$(HERE)/include -I$(HERE)/tests/host \
	  $(HOST_SRCS) -o $@

test-host: $(HOST_BIN)
	$(HOST_BIN)

# ---- checks -----------------------------------------------------------------

# Nothing from ps2sdk: no ps2sdk path in any link map, no include of a ps2sdk
# header (the compiler is never given -I$PS2SDK), and no known ps2sdk symbol.
check-deps: all
	@! grep -l -i "ps2sdk" $(OUT)/tests/*.map $(OUT)/probe/*.map $(OUT)/demos/*.map 2>/dev/null
	@! $(LLVM)/llvm-nm $(TESTS) $(PROBES) $(DEMOS) 2>/dev/null | \
	  grep -E " (SifInitRpc|sceSifInitRpc|_ps2sdk_|InitMainThread|_InitSys|_libcglue)"
	@echo "check-deps: no ps2sdk"

gen:
	python3 -I -B $(HERE)/gen/gen.py

clean:
	rm -rf $(OUT)

.PHONY: all batch libc picolibc test-host check-deps gen clean
.SECONDARY:
