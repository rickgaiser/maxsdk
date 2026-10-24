#!/usr/bin/env python3
"""A video of frames a program saved (rec_000.tga, rec_001.tga, ...):

    frames2video.py <dir> <out.gif|out.mp4> [--fps 30] [--prefix rec_]

GIF by Pillow; MP4 by ffmpeg (H.264).
"""

import argparse
import glob
import os
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("out")
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--prefix", default="rec_")
    args = ap.parse_args()
    frames = sorted(glob.glob(os.path.join(args.dir, args.prefix + "[0-9][0-9][0-9].tga")))
    if not frames:
        sys.exit("frames2video: no %s*.tga in %s" % (args.prefix, args.dir))
    if args.out.endswith(".mp4"):
        subprocess.check_call(["ffmpeg", "-loglevel", "error", "-y", "-framerate", str(args.fps),
                               "-i", os.path.join(args.dir, args.prefix + "%03d.tga"),
                               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", args.out])
    else:
        from PIL import Image
        ims = [Image.open(f).convert("RGB") for f in frames]
        ims[0].save(args.out, save_all=True, append_images=ims[1:], loop=0,
                    duration=int(round(1000 / args.fps)), optimize=False)
    print("frames2video: %d frames -> %s" % (len(frames), args.out))


if __name__ == "__main__":
    main()
