# maxsdk: build the library, tests and probes with one compiler.
#
#   make [COMPILER=clang|gcc] [OUT=<dir>]   library, test and probe ELFs
#   make test-host                          host tests (generated fields, packets)
#   make check-deps                         fail if anything links ps2sdk
#   make gen                                regenerate the hardware headers
#
# Output goes to toolchain/build/maxsdk/<compiler>/ by default.

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   := $(abspath $(HERE)/../../../..)
COMPILER ?= clang
OUT      ?= $(PS2MAX)/toolchain/build/maxsdk/$(COMPILER)
LLVM     ?= $(PS2MAX)/toolchain/build/llvm/bin
EE_BIN   ?= $(PS2MAX)/install/ee/bin
EE_GCC    = $(EE_BIN)/mips64r5900el-ps2-elf-gcc

HOSTCC   ?= cc

# -ffinite-math-only: the R5900 FPU has no INF/NaN, so clang only uses it for
# finite math; without the flag it calls soft-float helpers (not in libmx).
COMMON_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding \
                -fno-common -ffinite-math-only -I$(HERE)/include -G0
ifeq ($(COMPILER),gcc)
CC       = $(EE_GCC)
CFLAGS   = $(COMMON_CFLAGS) -mabi=n32 -fno-lto
# $(call link,out,objs)
link     = $(EE_GCC) -nostdlib -T$(HERE)/ld/maxsdk.ld -Wl,-zmax-page-size=128 -Wl,--gc-sections \
           -Wl,-Map,$(1:.elf=.map) -o $(1) $(2)
else
CC       = $(LLVM)/clang
CFLAGS   = $(COMMON_CFLAGS) --target=mips64el -mcpu=r5900 -mabi=n32 -fno-pic -mno-abicalls \
           -fomit-frame-pointer
link     = $(LLVM)/ld.lld -m elf32ltsmipn32 -T$(HERE)/ld/maxsdk.ld -z max-page-size=128 \
           --gc-sections -Map $(1:.elf=.map) -o $(1) $(2)
endif
CFLAGS  += $(EXTRA_CFLAGS)

LIB_SRCS := $(wildcard $(HERE)/src/*/*.c) $(wildcard $(HERE)/src/*/*.S)
LIB_OBJS := $(patsubst $(HERE)/%,$(OUT)/obj/%.o,$(LIB_SRCS))
LIB       = $(OUT)/libmx.a
CRT0      = $(OUT)/obj/crt0.o
HDRS     := $(wildcard $(HERE)/include/mx/*.h $(HERE)/include/mx/hw/*.h)

# VU microcode (*.vsm, assembled by llvm-mc -triple=vu): each becomes an EE
# object with the code between the symbols <name>_vu and <name>_vu_end.
VSMS     := $(wildcard $(HERE)/tests/ps2/*.vsm $(HERE)/probe/*.vsm $(HERE)/demos/*.vsm)
VU_OBJS  := $(patsubst %.vsm,$(OUT)/vu/%.o,$(notdir $(VSMS)))
vpath %.vsm $(HERE)/tests/ps2 $(HERE)/probe $(HERE)/demos

$(OUT)/vu/%.vu.elf: %.vsm
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-mc -triple=vu -mcpu=vu1 -filetype=obj $< -o $@

$(OUT)/vu/%.bin: $(OUT)/vu/%.vu.elf
	$(LLVM)/llvm-objcopy -O binary --only-section=.vutext $< $@

$(OUT)/vu/%.o: $(OUT)/vu/%.bin
	printf '%s\n' '.section .vutext, "a"' '.balign 16' '.globl $*_vu' '$*_vu:' \
	  '.incbin "$<"' '.globl $*_vu_end' '$*_vu_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@

TESTS    := $(patsubst $(HERE)/tests/ps2/%.c,$(OUT)/tests/%.elf,$(wildcard $(HERE)/tests/ps2/*.c))
PROBES   := $(patsubst $(HERE)/probe/%.c,$(OUT)/probe/%.elf,$(wildcard $(HERE)/probe/*.c))
DEMOS    := $(patsubst $(HERE)/demos/%.c,$(OUT)/demos/%.elf,$(wildcard $(HERE)/demos/*.c))

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
$(OUT)/tests/%.elf: $(HERE)/tests/ps2/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HDRS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(LIB))

$(OUT)/probe/%.elf: $(HERE)/probe/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HDRS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(LIB))

$(OUT)/demos/%.elf: $(HERE)/demos/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HDRS) $(HERE)/ld/maxsdk.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(LIB))

# ---- the combined PS2 batch ---------------------------------------------------
# One ELF that runs the tests, probes and demos below in a row, so a single
# PS2 run covers them. Each program's main() is renamed to <name>_main.

BATCH_SRCS := tests/ps2/dma.c tests/ps2/gs.c tests/ps2/vif.c tests/ps2/vu.c \
              probe/p2_dma.c probe/p3_gs.c probe/p4_vu.c demos/spin.c tests/ps2/dbit.c
BATCH_OBJS := $(patsubst %.c,$(OUT)/batch/%.o,$(BATCH_SRCS))

$(OUT)/batch/%.o: $(HERE)/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMX_BATCH -Dmain=$(notdir $*)_main -I$(HERE)/tests/ps2 -c $< -o $@

$(OUT)/batch/ps2batch.elf: $(HERE)/tests/batch/ps2batch.c $(BATCH_OBJS) $(LIB) $(CRT0) $(VU_OBJS)
	$(CC) $(CFLAGS) -DMX_BATCH -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(BATCH_OBJS) $(VU_OBJS) $(LIB))

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
	python3 -I $(HERE)/gen/gen.py

clean:
	rm -rf $(OUT)

.PHONY: all batch test-host check-deps gen clean
.SECONDARY:
