# maxsdk: build the library, tests, probes and examples with clang (the LLVM
# R5900 toolchain in this tree; gcc is not supported). Programs on maxsdk
# include mk/maxsdk.mk (the flags, the link and the VU and HLSL rules).
#
#   make [OUT=<dir>]                        library, test, probe and example ELFs
#   make lib                                libmx.a and crt0 only
#   make test-host                          host tests (generated fields, packets)
#   make check-deps                         fail if anything links ps2sdk
#   make gen                                regenerate the hardware headers
#
# Output goes to toolchain/build/maxsdk/clang/ by default.

HERE       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX     ?= $(abspath $(HERE)/../..)
OUT        ?= $(PS2MAX)/toolchain/build/maxsdk/clang
MAXSDK_OUT := $(OUT)
HOSTCC     ?= cc

LIB_SRCS := $(wildcard $(HERE)/src/*/*.c) $(wildcard $(HERE)/src/*/*.S)
LIB_OBJS := $(patsubst $(HERE)/%,$(OUT)/obj/%.o,$(LIB_SRCS))
LIB       = $(MX_LIB)
CRT0      = $(MX_CRT0)
HDRS      = $(MX_HDRS)
# the library's own VU microcode (src/*/*.vsm) goes into libmx.a
LIB_VSMS := $(wildcard $(HERE)/src/*/*.vsm)
LIB_OBJS += $(patsubst %.vsm,$(OUT)/vu/%.o,$(notdir $(LIB_VSMS)))
vpath %.vsm $(sort $(dir $(LIB_VSMS)))
# the VU0 renderer's feeder: its asm statements keep VF registers live
$(OUT)/obj/src/vu/vu0r.c.o: CFLAGS += -mvu0 $(foreach r,$(shell seq 1 31),-ffixed-vf$(r))

# Each example has a directory in examples/; the tests' and probes' VU and
# HLSL programs and the examples' share one namespace (VSM_DIRS, HLSL_DIRS).
# tests/ps2 has its own copies of the programs of apps that it tests (the
# model's m*.hlsl, procgen's lvl*, tf*, fbm, ...), probe/ of vu0skin.vsm.
EXAMPLE_DIRS := $(patsubst %/,%,$(wildcard $(HERE)/examples/*/))
VSM_DIRS     := $(HERE)/tests/ps2 $(HERE)/probe $(EXAMPLE_DIRS)
HLSL_DIRS    := $(HERE)/tests/ps2 $(EXAMPLE_DIRS)
HLSL_IX      := lvl0 lvl1 lvl2 mflat mlit mtex mscei mskin
HLSL_VU0     := fbm floor nest
include $(HERE)/mk/maxsdk.mk

TESTS    := $(patsubst $(HERE)/tests/ps2/%.c,$(OUT)/tests/%.elf,$(wildcard $(HERE)/tests/ps2/*.c))
PROBES   := $(patsubst $(HERE)/probe/%.c,$(OUT)/probe/%.elf,$(wildcard $(HERE)/probe/*.c))
EXAMPLES := $(patsubst %.c,$(OUT)/examples/%.elf,$(notdir $(wildcard $(HERE)/examples/*/*.c)))
vpath %.c $(EXAMPLE_DIRS)

# The apps still in this tree (demos/<name>, each with its own Makefile on
# mk/maxsdk.mk), built into $(OUT)/demos/<name>.
APPS     := $(notdir $(patsubst %/,%,$(wildcard $(HERE)/demos/*/)))

all: $(LIB) $(CRT0) $(TESTS) $(PROBES) $(EXAMPLES) $(OUT)/batch/ps2batch.elf apps
lib: $(LIB) $(CRT0)

apps: $(LIB) $(CRT0)
	for a in $(APPS); do $(MAKE) -C $(HERE)/demos/$$a OUT=$(OUT)/demos/$$a MAXSDK_OUT=$(OUT) || exit; done

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

# Each test, probe or example is one C file, linked with crt0 and libmx only
# (libmx has the compiler helper functions; no libgcc).
$(OUT)/tests/%.elf: $(HERE)/tests/ps2/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

$(OUT)/probe/%.elf: $(HERE)/probe/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PROBE_CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

# VU0 macro mode (COP2 instructions in inline asm)
$(OUT)/probe/p10_vucopy.elf: PROBE_CFLAGS = -mvu0
$(OUT)/probe/p11_vu0mix.elf: PROBE_CFLAGS = -mvu0
$(OUT)/probe/p15_vu0skin.elf: PROBE_CFLAGS = -mvu0 -fapprox-func
$(OUT)/probe/p16_vu0reg.elf: PROBE_CFLAGS = -mvu0 -fapprox-func
# all VF registers reserved: the VU0 programs it calls write most of them
$(OUT)/probe/p17_vu0c.elf: PROBE_CFLAGS = -mvu0 $(foreach r,$(shell seq 1 31),-ffixed-vf$(r))

# An example's C file finds the headers of its directory.
$(OUT)/examples/%.elf: %.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(MX_LD)
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
RT_DIR       ?= $(PS2MAX)/toolchain/generic/llvm/llvm-project/compiler-rt/lib/builtins
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

$(OUT)/libc/tests/%.elf: $(HERE)/tests/libc/%.c $(LIBMXC) $(LIB) $(CRT0) $(RT_BUILTINS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(LIBC_LIBS))

$(OUT)/libc/tests/%.elf: $(HERE)/tests/libc/%.cc $(LIBMXC) $(LIB) $(CRT0) $(RT_BUILTINS) $(MX_LD)
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
              examples/spin/spin.c examples/scene/scene.c examples/allunits/allunits.c tests/ps2/thread.c \
              tests/ps2/dbit.c
BATCH_OBJS := $(patsubst %.c,$(OUT)/batch/%.o,$(BATCH_SRCS))

$(OUT)/batch/%.o: $(HERE)/%.c $(HDRS) $(HLSL_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMX_BATCH -Dmain=$(notdir $*)_main -I$(HERE)/tests/ps2 -c $< -o $@

$(OUT)/batch/ps2batch.elf: $(HERE)/tests/batch/ps2batch.c $(BATCH_OBJS) $(LIB) $(CRT0) $(VU_OBJS) \
                           $(HLSL_OBJS) $(MX_LD)
	$(CC) $(CFLAGS) -DMX_BATCH -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(BATCH_OBJS) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

batch: $(OUT)/batch/ps2batch.elf

# ---- host tests -----------------------------------------------------------

# Pure parts of the library are tested on the host too.
HOST_SRCS := $(wildcard $(HERE)/tests/host/*.c) $(HERE)/src/pkt/pkt.c $(HERE)/src/pkt/chain.c \
             $(HERE)/src/gs/shader.c $(HERE)/src/gs/rt.c $(HERE)/src/gs/display.c \
             $(HERE)/src/gs/video.c $(HERE)/src/gs/tex.c
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
	@! grep -l -i "ps2sdk" $(OUT)/tests/*.map $(OUT)/probe/*.map $(OUT)/examples/*.map \
	  $(OUT)/demos/*/*.map 2>/dev/null
	@! $(LLVM)/llvm-nm $(TESTS) $(PROBES) $(EXAMPLES) $(wildcard $(OUT)/demos/*/*.elf) 2>/dev/null | \
	  grep -E " (SifInitRpc|sceSifInitRpc|_ps2sdk_|InitMainThread|_InitSys|_libcglue)"
	@echo "check-deps: no ps2sdk"

gen:
	python3 -I -B $(HERE)/gen/gen.py

clean:
	rm -rf $(OUT)

.PHONY: all lib apps batch libc picolibc test-host check-deps gen clean
