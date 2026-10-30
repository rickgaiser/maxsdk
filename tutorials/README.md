# maxsdk tutorials

Three tutorials for programmers who know the PlayStation 2 a little but are new to maxsdk.
Each is a series of small, complete programs, and each step adds one thing to the one before.
Read the step's text, then its source: every program is short and commented.

| | Tutorial | Steps |
|---|---|---|
| ![](1-basics/images/4_pad.png) | [1. The basics](1-basics/README.md) | a program that prints; the screen and the frame loop; drawing with GIF packets; the pad and text |
| ![](2-graphics/images/2_texture.png) | [2. Graphics](2-graphics/README.md) | triangles, flat and Gouraud; textures; alpha blending; the Z buffer |
| ![](3-vu1-hlsl/images/3_light.png) | [3. VU1 and HLSL](3-vu1-hlsl/README.md) | a vertex program; 3D with a matrix; light |

## Building

The tutorials are built like any program on maxsdk: their Makefile includes
[`mk/maxsdk.mk`](../mk/maxsdk.mk), which has the compiler flags, the link and the rules that
compile the HLSL programs for the VU. From the ps2max tree:

```
make -C libs/maxsdk/tutorials
```

This builds maxsdk's library if needed, then one ELF per step, in
`build/libs/maxsdk/tutorials/<tutorial>/<step>.elf`.

## Running

Every program prints to the EE's debug console and ends with a line
`@@PS2TEST_END@@ status=0`. Test runners wait for that line. Most steps also save a picture of
their last frame as `host:<step>.tga`. `host:` is the host computer's file system, and the
pictures in these pages are those files.

- **PCSX2:** run the ELF (`pcsx2-qt -elf <step>.elf`) with the setting Enable Host Filesystem
  on (`HostFs = true` in `PCSX2.ini`). `host:` is then the ELF's directory. The debug console
  is the EE console in the log window.
- **A PS2 with ps2link:** from the ELF's directory, `ps2client -h <PS2 address> execee
  host:<step>.elf`. `host:` is ps2client's working directory, and the console is ps2client's
  output.

`make shots` runs every step in PCSX2 with the software renderer, which draws like the PS2's
GS, and converts the pictures into the `images/` directories (`shots.sh`).

## What is where

| | |
|---|---|
| `tut.h` | what the programs share: `tut_screenshot` (the picture of the last frame) and `tut_sin`/`tut_cos` (there is no libm) |
| `<tutorial>/<k>_<step>.c` | the programs |
| `3-vu1-hlsl/*.hlsl`, `camera.h` | tutorial 3's vertex programs, and its matrices |
| `<tutorial>/images/` | the pictures (`make shots`) |

After the tutorials: the [examples](../examples/) (`spin`, `scene`, `display`, `vumax`), the
hardware notes in [docs/](../docs/README.md), and the model demo in `ps2max/apps/maxsdk_model`
(a skinned, animated model drawn by HLSL programs).
