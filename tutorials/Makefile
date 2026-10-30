# The tutorials (README.md). Each step is a program of its own,
# <tutorial>/<step>.c, built with mk/maxsdk.mk like any program on maxsdk.
#
#   make [OUT=<dir>]   every step's ELF, in $(OUT)/<tutorial>/<step>.elf
#   make shots         run each step in PCSX2 (software renderer) and put the
#                      picture it saves into <tutorial>/images/ (shots.sh)

all:

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX    ?= $(abspath $(HERE)/../../..)
OUT       ?= $(PS2MAX)/build/libs/maxsdk/tutorials
TUTORIALS := $(notdir $(patsubst %/,%,$(wildcard $(HERE)/[0-9]-*/)))
# the HLSL programs of tutorial 3
HLSL_DIRS := $(HERE)/3-vu1-hlsl
include $(HERE)/../mk/maxsdk.mk

STEPS := $(patsubst $(HERE)/%.c,$(OUT)/%.elf,$(foreach t,$(TUTORIALS),$(wildcard $(HERE)/$(t)/*.c)))

all: $(STEPS)

$(OUT)/%.elf: $(HERE)/%.c $(HERE)/tut.h $(wildcard $(HERE)/*/*.h) $(MX_LIB) $(MX_CRT0) $(HLSL_OBJS) $(HLSL_HDRS) $(MX_HDRS) \
              $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(HERE) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(MX_CRT0) $(@:.elf=.o) $(HLSL_OBJS) $(MX_LIB))

shots: $(STEPS)
	$(HERE)/shots.sh $(HERE) $(STEPS)

.PHONY: all shots
