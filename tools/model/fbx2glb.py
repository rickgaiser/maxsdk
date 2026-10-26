"""FBX to a small glTF (.glb) for gltf2mxm.py, run by Blender:

    blender -b --factory-startup -P fbx2glb.py -- in.fbx out.glb [options]

The meshes are decimated to about --tris triangles (in proportion, or
--ratio name=r for a mesh whose name contains name); --lods n: each mesh n
times, as <name>_LOD0 .. _LOD<n-1>, level k decimated to 2^-k of that (for
gltf2mxm.py: the levels of detail of one model), the textures are found
again by their file names under --textures (FBX files often hold the paths
of the machine they were made on) and scaled down to --tex-size, and the
action of --anim (an FBX of the same skeleton) is put on the armature: its
bones by name. --poses adds the poses of a blendposes.py file, each an
animation of one frame (or the animations of a blendcloth.py file) (the bone matrices of the rig it was made from, each
with the difference between the two rests). Only the bones that move
vertices are written, no shape keys.

Each material is made again with one texture, the base colour (all that
gltf2mxm uses): the image linked to Base Color, else one named *BaseColor*
in the material, else the name of one of its images (or of the material)
with the last _part made _BaseColor, e.g. Heels_Roughness -> Heels_BaseColor;
--basecolor MAT=file sets it. An image linked to Alpha that is not the
base colour (an opacity map) becomes the texture's alpha.
"""

import argparse
import json
import math
import os
import re
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix


def fail(msg):
    print("fbx2glb: " + msg)
    sys.exit(1)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser(prog="fbx2glb.py")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--anim", help="FBX with the animation (same bone names)")
    ap.add_argument("--poses", action="append", default=[], metavar="JSON",
                    help="poses made by blendposes.py, an animation each")
    ap.add_argument("--pose-tol", type=float, nargs=2, default=[1.0, 1.0],
                    metavar=("DEG", "MM"), help="the least turn and move of a bone in a pose")
    ap.add_argument("--strip-prefix", default="", metavar="TEXT",
                    help="taken off the start of the animations' names")
    ap.add_argument("--textures", help="directory searched for the textures by name")
    ap.add_argument("--tris", type=int, default=25000, help="triangles of the model")
    ap.add_argument("--ratio", action="append", default=[], metavar="NAME=R",
                    help="decimate ratio of the meshes whose name contains NAME")
    ap.add_argument("--lods", type=int, default=1,
                    help="levels of detail: each mesh n times, level k with 2^-k of the triangles")
    ap.add_argument("--exclude", action="append", default=[], metavar="NAME",
                    help="leave out the meshes whose name contains NAME")
    ap.add_argument("--drop-mat", action="append", default=[], metavar="NAME",
                    help="leave out the faces of material NAME")
    ap.add_argument("--basecolor", action="append", default=[], metavar="MAT=FILE",
                    help="the base colour texture of material MAT")
    ap.add_argument("--tex-size", type=int, default=512, help="largest texture side")
    ap.add_argument("--keep", action="append", default=[], metavar="NAME",
                    help="bones whose name contains NAME stay joints (spring joints)")
    ap.add_argument("--keep-bones", action="store_true",
                    help="with --anim: keep the bones it does not move (else their weights "
                         "go to the nearest bone it moves above them: the same pose)")
    args = ap.parse_args(argv)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=os.path.abspath(args.input))
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if len(arms) != 1:
        fail("%d armatures, not 1" % len(arms))
    arm = arms[0]

    for o in [o for o in bpy.data.objects if o.type == "MESH"]:
        if any(x in o.name for x in args.exclude):
            print("fbx2glb: %s left out" % o.name)
            bpy.data.objects.remove(o)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        fail("no meshes")

    # ---- faces of the dropped materials
    for o in meshes:
        drop = [i for i, m in enumerate(o.data.materials) if m and m.name in args.drop_mat]
        if not drop:
            continue
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index in drop],
                         context="FACES")
        bm.to_mesh(o.data)
        bm.free()

    # ---- textures, by their file names
    found = {}
    if args.textures:
        for d, _, files in os.walk(os.path.abspath(args.textures)):
            for f in files:
                found.setdefault(f.lower(), os.path.join(d, f))

    def base_name(path):
        return os.path.basename(path.replace("\\", "/"))

    def load(name):
        path = found.get(name.lower())
        if path is None:
            return None
        im = bpy.data.images.load(path, check_existing=True)
        w, h = im.size
        s = max(w, h)
        if s > args.tex_size:
            im.scale(max(1, w * args.tex_size // s), max(1, h * args.tex_size // s))
        return im

    forced = dict(b.split("=", 1) for b in args.basecolor)
    used = {m for o in meshes for m in o.data.materials if m}
    for mat in used:
        nt = mat.node_tree
        imgs, linked = [], {}
        for n in nt.nodes if nt else []:
            if n.type == "TEX_IMAGE" and n.image:
                nm = base_name(n.image.filepath)
                imgs.append(nm)
                for out in n.outputs:
                    for l in out.links:
                        linked.setdefault(l.to_socket.name, nm)
        cand = []
        if mat.name in forced:
            cand.append(forced[mat.name])
        if "Base Color" in linked:
            cand.append(linked["Base Color"])
        cand += [nm for nm in imgs if "basecolor" in nm.lower()]
        for nm in imgs + [mat.name + "_x.tga"]:
            stem, ext = os.path.splitext(nm)
            cand += [re.sub(r"_[^_]+$", "_BaseColor", stem) + e for e in (ext, ".tga", ".png")]
        base = next((c for c in cand if c.lower() in found), None)
        alpha = linked.get("Alpha")
        if alpha == base or (alpha and alpha.lower() not in found):
            alpha = None
        im = load(base) if base else None
        if im is None:
            print("fbx2glb: material %s: no base colour texture" % mat.name)
        elif alpha:
            # the opacity map as the alpha of a copy of the base colour
            am = load(alpha)
            w, h = im.size
            if tuple(am.size) != (w, h):
                am.scale(w, h)
            px = np.empty(w * h * 4, np.float32)
            im.pixels.foreach_get(px)
            ap_ = np.empty(w * h * 4, np.float32)
            am.pixels.foreach_get(ap_)
            px[3::4] = ap_[0::4]
            im = bpy.data.images.new(os.path.splitext(base)[0] + "_A", w, h, alpha=True)
            im.pixels.foreach_set(px)
        # the mean roughness and metalness of its maps (the highlight's strength)
        def mean_of(socket, part):
            nm = linked.get(socket) or next((n for n in imgs if part in n.lower()), None)
            if not nm or nm.lower() not in found:
                return None
            ri = bpy.data.images.load(found[nm.lower()], check_existing=True)
            if ri.size[0] > 64:
                ri.scale(64, 64)
            px = np.empty(ri.size[0] * ri.size[1] * 4, np.float32)
            ri.pixels.foreach_get(px)
            return float(px[0::4].mean())
        rough = mean_of("Roughness", "_roughness")
        metal = mean_of("Metallic", "_metallic")
        print("fbx2glb: material %s: %s%s, roughness %s, metallic %s" %
              (mat.name, base, " + alpha " + alpha if alpha else "",
               "-" if rough is None else "%.2f" % rough, "-" if metal is None else "%.2f" % metal))
        mat.use_nodes = True
        nt = mat.node_tree
        nt.nodes.clear()
        bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
        bsdf.inputs["Roughness"].default_value = 0.6 if rough is None else rough
        bsdf.inputs["Metallic"].default_value = 0.0 if metal is None else metal
        outn = nt.nodes.new("ShaderNodeOutputMaterial")
        nt.links.new(bsdf.outputs["BSDF"], outn.inputs["Surface"])
        if im:
            tn = nt.nodes.new("ShaderNodeTexImage")
            tn.image = im
            nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
            if alpha:
                nt.links.new(tn.outputs["Alpha"], bsdf.inputs["Alpha"])
    for im in bpy.data.images:
        # written as PNG into the .glb
        im.file_format = "PNG"

    # ---- the animation, by bone names
    animated = None
    if args.anim:
        before = set(bpy.data.objects)
        acts_before = set(bpy.data.actions)
        bpy.ops.import_scene.fbx(filepath=os.path.abspath(args.anim))
        new = set(bpy.data.objects) - before
        src = [o for o in new if o.type == "ARMATURE"]
        if len(src) != 1:
            fail("%s: %d armatures, not 1" % (args.anim, len(src)))
        src = src[0]
        act = src.animation_data.action if src.animation_data else None
        if act is None:
            acts = [a for a in set(bpy.data.actions) - acts_before if a.name.startswith(src.name)]
            act = acts[0] if acts else None
        if act is None:
            fail("%s: no armature action" % args.anim)
        names = {b.name for b in arm.data.bones}
        src_names = {b.name for b in src.data.bones}
        print("fbx2glb: anim %s: %d of its %d bones are in the model's %d" %
              (act.name, len(src_names & names), len(src_names), len(names)))
        animated = src_names & names
        # "armature|action": the action's own name
        act.name = act.name.split("|")[-1]
        arm.animation_data_create()
        arm.animation_data.action = act
        if hasattr(act, "slots") and act.slots:
            arm.animation_data.action_slot = act.slots[0]
        for o in new:
            bpy.data.objects.remove(o)
        sc = bpy.context.scene
        sc.frame_start, sc.frame_end = int(act.frame_range[0]), int(act.frame_range[1])
        for a in list(bpy.data.actions):
            if a != act:
                bpy.data.actions.remove(a)

    # ---- the poses: the armature-space matrix of each bone (the rig's, with
    # the difference of the rests; a bone without one keeps its place to its
    # parent), as the local pose of one frame
    actions = [arm.animation_data.action] if arm.animation_data and arm.animation_data.action \
        else []
    aw = arm.matrix_world
    awi = aw.inverted()
    # a bone moved less than --pose-tol by every pose is not one it moves
    rot_tol, loc_tol = math.radians(args.pose_tol[0]), args.pose_tol[1] / 1000
    for path in args.poses:
        pj = json.load(open(path))
        rest_src = {n: Matrix(m) for n, m in pj["rest"].items()}
        if animated is None:
            animated = set()
        for p in pj["poses"]:
            act = bpy.data.actions.new(p["name"].split("|")[-1])
            arm.animation_data_create()
            arm.animation_data.action = act
            if getattr(act, "slots", None) is not None and not act.slots:
                arm.animation_data.action_slot = act.slots.new("OBJECT", arm.name)
            # a pose, or an animation of many frames (blendcloth.py) at its fps
            if "fps" in p:
                bpy.context.scene.render.fps = int(round(p["fps"]))
                bpy.context.scene.render.fps_base = 1.0
            for fi, bones in enumerate(p.get("frames") or [p["bones"]]):
                A = {}
                for b in arm.data.bones:  # parents before children
                    if b.name in bones:
                        A[b.name] = (awi @ Matrix(bones[b.name]) @ rest_src[b.name].inverted() @
                                     aw @ b.matrix_local)
                    elif b.parent:
                        A[b.name] = (A[b.parent.name] @ b.parent.matrix_local.inverted() @
                                     b.matrix_local)
                    else:
                        A[b.name] = b.matrix_local.copy()
                for pb in arm.pose.bones:
                    b = pb.bone
                    if b.parent:
                        basis = ((b.parent.matrix_local.inverted() @ b.matrix_local).inverted() @
                                 A[b.parent.name].inverted() @ A[b.name])
                    else:
                        basis = b.matrix_local.inverted() @ A[b.name]
                    loc, rot, scale = basis.decompose()
                    if rot.angle > rot_tol or loc.length > loc_tol:
                        animated.add(b.name)
                    pb.rotation_mode = "QUATERNION"
                    pb.location, pb.rotation_quaternion, pb.scale = loc, rot, scale
                    for prop in ("location", "rotation_quaternion", "scale"):
                        pb.keyframe_insert(prop, frame=fi + 1)
            actions.append(act)
        print("fbx2glb: %d poses of %s" % (len(pj["poses"]), path))
    # every action an animation: each in an NLA track
    for act in actions:
        if args.strip_prefix and act.name.startswith(args.strip_prefix):
            act.name = act.name[len(args.strip_prefix):]
    if len(actions) > 1:
        ad = arm.animation_data
        ad.action = None
        for act in actions:
            act.use_fake_user = True
            st = ad.nla_tracks.new().strips.new(act.name, int(act.frame_range[0]), act)
            if getattr(act, "slots", None):
                st.action_slot = act.slots[0]
        print("fbx2glb: animations %s" % ", ".join(a.name for a in actions))

    # ---- decimation: before the armature modifier
    def tris_of(o):
        return sum(len(p.vertices) - 2 for p in o.data.polygons)
    total = sum(tris_of(o) for o in meshes)
    base = min(1.0, args.tris / total)
    fixed = {}
    for r in args.ratio:
        k, v = r.split("=")
        fixed[k] = float(v)
    level = {o.name: 0 for o in meshes}
    if args.lods > 1:
        # the levels: copies of each mesh (its modifiers, vertex groups and
        # parent come along), named by their level
        for o in list(meshes):
            name = o.name
            for k in range(1, args.lods):
                c = o.copy()
                c.data = o.data.copy()
                for col in o.users_collection:
                    col.objects.link(c)
                c.name = "%s_LOD%d" % (name, k)
                level[c.name] = k
                meshes.append(c)
            o.name = "%s_LOD0" % name
            level[o.name] = 0
    for o in meshes:
        ratio = next((v for k, v in fixed.items() if k in o.name), base) * 0.5 ** level[o.name]
        if ratio >= 1.0:
            continue
        bpy.context.view_layer.objects.active = o
        # the decimate modifier cannot be applied to a mesh with shape keys
        if o.data.shape_keys:
            o.shape_key_clear()
        m = o.modifiers.new("decimate", "DECIMATE")
        m.ratio = ratio
        m.use_collapse_triangulate = True
        bpy.ops.object.modifier_move_to_index(modifier=m.name, index=0)
        bpy.ops.object.modifier_apply(modifier=m.name)
    # ---- the weights of the bones the animation does not move: to the
    # nearest one above them that it moves (they keep their place to it)
    if animated is not None and args.keep:
        animated |= {b.name for b in arm.data.bones if any(k in b.name for k in args.keep)}
    if animated and not args.keep_bones:
        def target(name):
            b = arm.data.bones.get(name)
            while b and b.name not in animated:
                b = b.parent
            return b.name if b else None
        for o in meshes:
            moves = {}
            for g in o.vertex_groups:
                t = target(g.name) if g.name in arm.data.bones else None
                if t and t != g.name:
                    moves[g.index] = t
            if not moves:
                continue
            dst = {}
            for t in set(moves.values()):
                dst[t] = o.vertex_groups.get(t) or o.vertex_groups.new(name=t)
            for v in o.data.vertices:
                add = {}
                for ge in v.groups:
                    if ge.group in moves and ge.weight > 0:
                        add[moves[ge.group]] = add.get(moves[ge.group], 0.0) + ge.weight
                for t, w in add.items():
                    dst[t].add([v.index], w, "ADD")
            for gi in sorted(moves, reverse=True):
                o.vertex_groups.remove(o.vertex_groups[gi])

    # ---- the bones that move vertices (and their ancestors, for the hierarchy)
    weighted = set()
    for o in meshes:
        names = {g.index: g.name for g in o.vertex_groups}
        for v in o.data.vertices:
            for g in v.groups:
                if g.weight > 0 and g.group in names:
                    weighted.add(names[g.group])
    keep = set()
    for b in arm.data.bones:
        if b.name in weighted:
            p = b
            while p and p.name not in keep:
                keep.add(p.name)
                p = p.parent
    for b in arm.data.bones:
        b.use_deform = b.name in keep
    print("fbx2glb: %d of %d bones" % (len(keep), len(arm.data.bones)))

    after = sum(tris_of(o) for o in meshes)
    for o in meshes:
        print("fbx2glb: %-32s %6d triangles" % (o.name, tris_of(o)))
    print("fbx2glb: %d triangles of %d" % (after, total))

    bpy.ops.export_scene.gltf(
        filepath=os.path.abspath(args.output), export_format="GLB",
        export_texcoords=True, export_normals=True, export_tangents=False,
        export_materials="EXPORT", export_image_format="AUTO",
        export_skins=True, export_def_bones=True, export_morph=False,
        export_animations=bool(actions), export_animation_mode="ACTIONS",
        export_force_sampling=True,
        export_apply=False, export_yup=True)
    print("fbx2glb: %s, %d bytes" % (args.output, os.path.getsize(args.output)))


main()
