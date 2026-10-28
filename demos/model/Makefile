# The model demo, on maxsdk (mk/maxsdk.mk).
#
#   make [OUT=<dir>]          model.elf, model_shots.elf, model_mipshots.elf
#   make model-record         model_rec1.elf and model_rec8.elf

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MAXSDK    ?= $(abspath $(HERE)/../..)
PS2MAX    ?= $(abspath $(MAXSDK)/../..)
OUT       ?= $(PS2MAX)/toolchain/build/maxsdk/clang/demos/model
VSM_DIRS  := $(HERE)
HLSL_DIRS := $(HERE)
HLSL_IX   := mflat mlit mtex mscei mskin
include $(MAXSDK)/mk/maxsdk.mk

MXM_TOOLS ?= $(MAXSDK)/tools/model
DEPS       = $(MX_LIB) $(MX_CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(MX_HDRS) $(MX_LD) \
             $(HERE)/mxm.h $(OUT)/fox_mxm.o
OBJS       = $(VU_OBJS) $(HLSL_OBJS) $(OUT)/fox_mxm.o $(MX_LIB)
# VU0 macro mode; approximate functions for VRSQRT (the skinning's normals)
CFLAGS    += -mvu0 -fapprox-func -I$(HERE)

all: $(OUT)/model.elf $(OUT)/model_shots.elf $(OUT)/model_mipshots.elf

# The model, built in (gltf2mxm.py: Python 3 and Pillow), made for the indexed
# programs it draws with.
MODEL_IX := $(patsubst %,$(OUT)/hlsl/%_ix.h,mflat mlit mtex mscei)
$(OUT)/fox.mxm: $(HERE)/assets/Fox.glb $(MXM_TOOLS)/gltf2mxm.py $(MODEL_IX)
	@mkdir -p $(dir $@)
	python3 -I -B $(MXM_TOOLS)/gltf2mxm.py $< $@ --limits $(MODEL_IX)

$(OUT)/%_mxm.o: $(OUT)/%.mxm
	printf '%s\n' '.section .rodata.model, "a"' '.balign 64' '.globl model_$*' 'model_$*:' \
	  '.incbin "$<"' '.globl model_$*_end' 'model_$*_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@

$(OUT)/model.elf: $(HERE)/model.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(MX_CRT0) $(@:.elf=.o) $(OBJS))

# model_shots.elf: still pictures of the model with each AA1 and supersampling
# mode as host:shot_*.tga (MODEL_SHOTS).
$(OUT)/model_shots.elf: $(HERE)/model.c $(DEPS)
	$(CC) $(CFLAGS) -DMODEL_SHOTS -c $< -o $(@:.elf=.o)
	$(call link,$@,$(MX_CRT0) $(@:.elf=.o) $(OBJS))

# model_mipshots.elf: still pictures of the model in each MIPMAP mode at four
# distances as host:mip_*.tga (MODEL_MIPSHOTS).
$(OUT)/model_mipshots.elf: $(HERE)/model.c $(DEPS)
	$(CC) $(CFLAGS) -DMODEL_MIPSHOTS -c $< -o $(@:.elf=.o)
	$(call link,$@,$(MX_CRT0) $(@:.elf=.o) $(OBJS))

# model_rec<n>.elf: the model demo recording MODEL_RECORD frames of n x n
# copies as host:rec_*.tga (tools/model/frames2video.py makes a video of them).
MODEL_RECORD ?= 90
$(OUT)/model_rec%.elf: $(HERE)/model.c $(DEPS)
	$(CC) $(CFLAGS) -DMODEL_RECORD=$(MODEL_RECORD) -DMODEL_RECORD_COPIES=$* -c $< -o $(@:.elf=.o)
	$(call link,$@,$(MX_CRT0) $(@:.elf=.o) $(OBJS))

model-record: $(OUT)/model_rec1.elf $(OUT)/model_rec8.elf

clean:
	rm -rf $(OUT)

.PHONY: all model-record clean
