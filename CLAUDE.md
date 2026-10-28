# maxsdk: what belongs here

maxsdk is the hardware SDK: only the basics for high-performance PS2 programs. Models, games
and their shaders live in other repos under `ps2max/` (split 2026-10-28). Keep it that way.

| Repo | Holds | Does not hold |
|---|---|---|
| `libs/maxsdk` (here) | units, DMA, packets, GS, VU control, the frame pipeline and every route a draw can take (VIF1 stream, chain calls, PATH3, EE + VU0 `vu0r`), `struct mx_shader` and the chain builders for it, textures, HUD, sched, threads, libc glue, `mk/maxsdk.mk` | model formats, skinning, animation, app shaders, app code |
| `libs/mxm` | the MXM format, its converter (`tools/gltf2mxm.py`), and the PS2 side of MXM models: loading, LOD, skinning, animation, building DMA chains for an app's shader. It builds chains, never sends them. | pipeline set-up, sending, shaders |
| `apps/model`, `apps/procgen` | the HLSL shaders, which route a model takes, sending, and app-only techniques (supersampling tiles, culling, terrain) | anything a second app would reuse: move that to `libs/mxm` or here |

Rules for changes here:
- Nothing in maxsdk includes or names a model format, an app, or an app's shader.
- New hardware or pipeline features go here; model or scene logic goes to `libs/mxm`; the look
  (shaders) and the game go to an app.
- `examples/` are small SDK programs (one C file, maybe its own VU programs). No apps here: an
  app gets its own repo under `ps2max/apps/` with a Makefile that includes `mk/maxsdk.mk`.
- `tests/ps2/*.hlsl` and `probe/vu0skin.vsm` are frozen copies of app programs, kept as test
  fixtures. Don't point tests at the apps' files, and don't treat these copies as the apps'
  shaders.
- Build output never goes into a source tree.

If a change seems to need one of these crossings, ask the user first.
