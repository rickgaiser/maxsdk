"""The poses (actions) of a .blend file's armature as bone matrices, for
fbx2glb.py --poses; run by Blender:

    blender -b in.blend -P blendposes.py -- out.json [--target in.fbx] [options]

Each action of the armature is put on it and evaluated at its first frame
(or --frame), with the rig's constraints and drivers: the world matrix of
every bone, and of every bone at rest. A control rig (Auto-Rig Pro, Rigify)
is evaluated as it is, so its deforming bones have the pose that its
controls give them. Actions of other things (shape keys) are left out.

--target: the bones are those of the skeleton of that FBX (an export of the
rig, often with other bone names): each is the deforming bone of the rig
with the same head and tail at rest (within --tol), else the one of the
same name.
"""

import argparse
import json
import re
import sys

import bpy


def fcurves(a):
    if getattr(a, "layers", None):
        for l in a.layers:
            for s in l.strips:
                for cb in s.channelbags:
                    yield from cb.fcurves
    else:
        yield from getattr(a, "fcurves", [])


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser(prog="blendposes.py")
    ap.add_argument("output")
    ap.add_argument("--armature", help="the armature (else the one with the most bones)")
    ap.add_argument("--frame", type=float, help="the frame of each action (else its first)")
    ap.add_argument("--target", help="FBX whose skeleton gets the poses (bones matched by place)")
    ap.add_argument("--tol", type=float, default=0.002, help="largest distance of a match (m)")
    ap.add_argument("--map", action="append", default=[], metavar="TARGET=SOURCE",
                    help="the rig's bone SOURCE for the target's bone TARGET")
    args = ap.parse_args(argv)

    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if args.armature:
        arms = [o for o in arms if o.name == args.armature]
    if not arms:
        sys.exit("blendposes: no armature")
    arm = max(arms, key=lambda o: len(o.data.bones))
    names = {b.name for b in arm.data.bones}

    def mat(m):
        return [list(r) for r in m]

    # ---- the bones written: the target's, each from a bone of the rig
    src_of = {n: n for n in names}
    if args.target:
        before = set(bpy.data.objects)
        bpy.ops.import_scene.fbx(filepath=args.target)
        tgt = [o for o in set(bpy.data.objects) - before if o.type == "ARMATURE"][0]
        tw = tgt.matrix_world
        deform = [b for b in arm.data.bones if b.use_deform]
        aw = arm.matrix_world
        ends = {b.name: (aw @ b.head_local, aw @ b.tail_local) for b in deform}
        src_of, far = {}, []
        forced = dict(m.split("=", 1) for m in args.map)
        for b in tgt.data.bones:
            if forced.get(b.name) in names:
                src_of[b.name] = forced[b.name]
                continue
            h, t = tw @ b.head_local, tw @ b.tail_local
            best = min(ends, key=lambda n: (ends[n][0] - h).length + (ends[n][1] - t).length)
            d = (ends[best][0] - h).length + (ends[best][1] - t).length
            if d > args.tol:
                # the head alone (the tails of an export can differ)
                best = min(ends, key=lambda n: (ends[n][0] - h).length)
                d = (ends[best][0] - h).length
            if d <= args.tol:
                src_of[b.name] = best
            elif b.name in names:
                src_of[b.name] = b.name
            else:
                far.append((b.name, best, d))
        print("blendposes: %d of the %d bones of %s matched, %d not: %s" %
              (len(src_of), len(tgt.data.bones), tgt.name, len(far),
               ", ".join("%s (%s %.4f)" % f for f in far[:12])))
        for o in set(bpy.data.objects) - before:
            bpy.data.objects.remove(o)

    if arm.animation_data is None:
        arm.animation_data_create()
    arm.animation_data.action = None
    # the NLA would add to the actions
    for t in arm.animation_data.nla_tracks:
        t.mute = True
    bpy.context.scene.frame_set(1)
    bones = arm.data.bones
    rest = {n: mat(arm.matrix_world @ bones[s].matrix_local) for n, s in src_of.items()}
    poses = []
    for a in bpy.data.actions:
        bones = {m.group(1) for fc in fcurves(a)
                 for m in [re.match(r'pose\.bones\["(.+?)"\]', fc.data_path)] if m}
        if not bones & names:
            continue
        arm.animation_data.action = a
        if getattr(a, "slots", None):
            arm.animation_data.action_slot = a.slots[0]
        f = args.frame if args.frame is not None else a.frame_range[0]
        bpy.context.scene.frame_set(int(f), subframe=f - int(f))
        bpy.context.view_layer.update()
        pb = arm.pose.bones
        pose = {n: mat(arm.matrix_world @ pb[s].matrix) for n, s in src_of.items()}
        poses.append({"name": a.name, "bones": pose})
        print("blendposes: %s (frame %g, %d bones keyed)" % (a.name, f, len(bones & names)))
    arm.animation_data.action = None
    json.dump({"armature": arm.name, "rest": rest, "poses": poses}, open(args.output, "w"))
    print("blendposes: %d poses of %s, %d bones" % (len(poses), arm.name, len(names)))


main()
