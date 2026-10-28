# The procgen demo (procgen.elf) and the level test (levels.elf), on maxsdk
# (mk/maxsdk.mk).
#
#   make [OUT=<dir>]

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MAXSDK    ?= $(abspath $(HERE)/../..)
PS2MAX    ?= $(abspath $(MAXSDK)/../..)
OUT       ?= $(PS2MAX)/toolchain/build/maxsdk/clang/demos/procgen
VSM_DIRS  := $(HERE)
HLSL_DIRS := $(HERE)
HLSL_IX   := lvl0 lvl1 lvl2
HLSL_VU0  := fbm dust
include $(MAXSDK)/mk/maxsdk.mk

CFLAGS += -I$(HERE)

all: $(OUT)/procgen.elf $(OUT)/levels.elf

$(OUT)/%.elf: $(HERE)/%.c $(MX_LIB) $(MX_CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(MX_HDRS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(MX_CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(MX_LIB))

clean:
	rm -rf $(OUT)

.PHONY: all clean
