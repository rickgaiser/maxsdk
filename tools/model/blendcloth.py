"""An animation with the cloth simulation of a .blend file baked into bone
motion, for fbx2glb.py --poses; run by Blender:

    blender -b rig.blend -P blendcloth.py -- out.json --target skel.fbx --anim walk.fbx
        [--cycle N] [--loops N] [--cloth NAME ...] [--map TARGET=SOURCE ...]

The .blend has the control rig (Auto-Rig Pro), meshes that follow cloth
"cages" (Cloth modifier on a hidden mesh, Surface Deform on the visible one)
and the cages' colliders. The animation of --anim (an FBX of the skeleton of
--target) is put on the rig's deforming bones, each matched to a bone of the
skeleton by its head and tail at rest (or --map), their constraints muted;
--loops cycles of --cycle frames are simulated in order, so the cloth is in
its loop by the last. For the last cycle, each bone whose name contains a
--cloth NAME gets a rigid motion (the other bones keep the animation's):
first the one that best takes the vertices it moves most (in the meshes with
Surface Deform) from the rest pose to the simulation (least squares,
Kabsch), then --iters times for each bone the one that best takes its
vertices where the simulation has them less what the other bones move them
(each vertex as much as its weight: the skinning of all the bones together
fitted to the simulation; each bone also kept to its own vertices, --anchor,
so that it does not turn far while its neighbours make up for it, and
--smooth to where it took them the frame before). The
cycle before the last is fitted too, and the last --blend frames are blended
into it: the end of the loop meets its start. A cloth bone's single frame
far from both neighbours (--spike degrees) is their mean. The error of both fits and the
cloth bones' turns between frames are printed. Written: the
skeleton's bones at rest and per frame (world matrices), as blendposes.py.
"""

import argparse
import json
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix


def mat(m):
    return [list(r) for r in m]


def kabsch(r, e, w):
    """The rotation R and move t that take the points r closest to e (each
    as much as w): least squares."""
    w = w / w.sum()
    rc, ec = w @ r, w @ e
    H = ((r - rc) * w[:, None]).T @ (e - ec)
    U, _, Vt = np.linalg.svd(H)
    d = np.sign(np.linalg.det(Vt.T @ U.T))
    R = Vt.T @ np.diag([1, 1, d]) @ U.T
    return R, ec - R @ rc


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser(prog="blendcloth.py")
    ap.add_argument("output")
    ap.add_argument("--target", required=True, help="FBX of the skeleton written")
    ap.add_argument("--anim", required=True, help="FBX with the animation (that skeleton)")
    ap.add_argument("--armature", help="the rig (else the armature with the most bones)")
    ap.add_argument("--cycle", type=int, default=40, help="frames of a cycle of the animation")
    ap.add_argument("--loops", type=int, default=6, help="cycles simulated")
    ap.add_argument("--cloth", nargs="+", default=["dress_", "breast_", "butt_"],
                    help="bones whose motion comes from the simulation")
    ap.add_argument("--name", default="Walk", help="the name of the animation written")
    ap.add_argument("--iters", type=int, default=8,
                    help="rounds of fitting the cloth bones together (0: each on its own)")
    ap.add_argument("--anchor", type=float, default=0.3,
                    help="fitting together, how much each bone keeps to its own fit (the "
                         "vertices it moves most, as a share of its weighted vertices)")
    ap.add_argument("--blend", type=int, default=40,
                    help="the last frames blended into the cycle before (the loop closed)")
    ap.add_argument("--smooth", type=float, default=1.0,
                    help="fitting together, how much each bone keeps to where it took its "
                         "own vertices the frame before (as --anchor)")
    ap.add_argument("--spike", type=float, default=40,
                    help="a cloth bone's turn (degrees) from both of its neighbour frames "
                         "that are within it of each other: smoothed")
    ap.add_argument("--tol", type=float, default=0.002, help="largest distance of a match (m)")
    ap.add_argument("--map", action="append", default=[], metavar="TARGET=SOURCE",
                    help="the rig's bone SOURCE for the skeleton's bone TARGET")
    args = ap.parse_args(argv)

    sc = bpy.context.scene
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if args.armature:
        arms = [o for o in arms if o.name == args.armature]
    rig = max(arms, key=lambda o: len(o.data.bones))
    rw = rig.matrix_world.copy()
    if rig.animation_data:
        rig.animation_data.action = None
        for t in rig.animation_data.nla_tracks:
            t.mute = True

    # ---- the skeleton, and its bones matched to the rig's deforming ones
    def import_armature(path, keep_meshes=False):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.fbx(filepath=os.path.abspath(path))
        new = set(bpy.data.objects) - before
        arm = [o for o in new if o.type == "ARMATURE"][0]
        kept = []
        for o in new:
            if o == arm:
                continue
            if keep_meshes and o.type == "MESH":
                for m in list(o.modifiers):
                    o.modifiers.remove(m)
                o.hide_viewport = o.hide_render = True
                kept.append(o)
            else:
                bpy.data.objects.remove(o)
        return arm, kept
    skel, skel_meshes = import_armature(args.target, True)
    sw = skel.matrix_world.copy()
    rest = {b.name: sw @ b.matrix_local for b in skel.data.bones}
    deform = [b for b in rig.data.bones if b.use_deform]
    ends = {b.name: (rw @ b.head_local, rw @ b.tail_local) for b in deform}
    forced = dict(m.split("=", 1) for m in args.map)
    src_of = {}
    for b in skel.data.bones:
        if b.name in forced and forced[b.name] in rig.data.bones:
            src_of[b.name] = forced[b.name]
            continue
        h, t = sw @ b.head_local, sw @ b.tail_local
        best = min(ends, key=lambda n: (ends[n][0] - h).length + (ends[n][1] - t).length)
        if (ends[best][0] - h).length + (ends[best][1] - t).length > args.tol:
            best = min(ends, key=lambda n: (ends[n][0] - h).length)
            if (ends[best][0] - h).length > args.tol:
                best = b.name if b.name in ends else None
        if best:
            src_of[b.name] = best
    rig_of = {}
    for n, s in src_of.items():
        rig_of.setdefault(s, n)
    print("blendcloth: %d of the %d bones of the skeleton matched" %
          (len(src_of), len(skel.data.bones)))

    # ---- the animation's world matrices of the skeleton per frame of a cycle
    # (the cloth and the meshes' modifiers off meanwhile)
    mods = [m for o in bpy.data.objects if o.type == "MESH" for m in o.modifiers
            if m.type in ("CLOTH", "SURFACE_DEFORM", "CORRECTIVE_SMOOTH", "NODES")]
    for m in mods:
        m.show_viewport = False
    walk, _ = import_armature(args.anim)
    act = walk.animation_data.action if walk.animation_data else None
    if act is None:
        sys.exit("blendcloth: %s: no armature action" % args.anim)
    f0 = int(act.frame_range[0])
    ww = walk.matrix_world
    order = [b.name for b in skel.data.bones]  # parents before children
    frames = []
    for f in range(args.cycle):
        sc.frame_set(f0 + f)
        bpy.context.view_layer.update()
        W = {}
        for n in order:
            pb = walk.pose.bones.get(n)
            par = skel.data.bones[n].parent
            if pb is not None:
                W[n] = ww @ pb.matrix
            elif par is not None:
                W[n] = W[par.name] @ rest[par.name].inverted() @ rest[n]
            else:
                W[n] = rest[n].copy()
        frames.append(W)
    bpy.data.objects.remove(walk)

    # ---- the rig's deforming bones keyed to it (constraints muted), every
    # frame of loops cycles
    sc.frame_set(1)
    bpy.context.view_layer.update()
    static = {pb.name: pb.matrix.copy() for pb in rig.pose.bones}
    keyed = [b for b in rig.data.bones if b.name in rig_of]
    keyset = {b.name for b in keyed}
    for b in keyed:
        b.inherit_scale = "FULL"
        b.use_inherit_rotation = True
        pb = rig.pose.bones[b.name]
        for c in pb.constraints:
            c.mute = True
        pb.rotation_mode = "QUATERNION"
    rwi = rw.inverted()
    nframe = args.cycle * args.loops
    rig.animation_data_create()
    ra = bpy.data.actions.new("blendcloth")
    rig.animation_data.action = ra
    if getattr(ra, "slots", None) is not None and not ra.slots:
        rig.animation_data.action_slot = ra.slots.new("OBJECT", rig.name)
    for b in keyed:
        rig.pose.bones[b.name].keyframe_insert("location", frame=1)
        rig.pose.bones[b.name].keyframe_insert("rotation_quaternion", frame=1)
        rig.pose.bones[b.name].keyframe_insert("scale", frame=1)
    curves = {}

    def fcurves(a):
        if getattr(a, "layers", None):
            for l in a.layers:
                for s in l.strips:
                    for cb in s.channelbags:
                        yield from cb.fcurves
        else:
            yield from a.fcurves
    for fc in fcurves(ra):
        n = fc.data_path.split('"')[1]
        curves[(n, fc.data_path.rsplit(".", 1)[1], fc.array_index)] = fc
    vals = {k: [] for k in curves}
    for f in range(nframe):
        W = frames[f % args.cycle]
        A = {}
        for b in rig.data.bones:  # parents before children
            if b.name in keyset:
                n = rig_of[b.name]
                A[b.name] = rwi @ W[n] @ rest[n].inverted() @ rw @ b.matrix_local
        for b in keyed:
            p = b.parent
            P = (A[p.name] if p.name in A else static[p.name]) if p else None
            if p:
                basis = (p.matrix_local.inverted() @ b.matrix_local).inverted() @ P.inverted() @ A[b.name]
            else:
                basis = b.matrix_local.inverted() @ A[b.name]
            loc, rot, scl = basis.decompose()
            for i in range(3):
                vals[(b.name, "location", i)] += [f + 1, loc[i]]
                vals[(b.name, "scale", i)] += [f + 1, scl[i]]
            for i in range(4):
                vals[(b.name, "rotation_quaternion", i)] += [f + 1, rot[i]]
    for k, fc in curves.items():
        fc.keyframe_points.clear()
        fc.keyframe_points.add(nframe)
        fc.keyframe_points.foreach_set("co", vals[k])
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"
        fc.update()
    print("blendcloth: %d bones of the rig keyed, %d frames" % (len(keyed), nframe))

    # ---- the cloth simulated in order; the last cycle fitted
    for m in mods:
        m.show_viewport = True
    for o in bpy.data.objects:
        for m in o.modifiers:
            if m.type == "CLOTH":
                m.point_cache.frame_start, m.point_cache.frame_end = 1, nframe
    sc.frame_start, sc.frame_end = 1, nframe
    meshes = [o for o in bpy.data.objects if o.type == "MESH" and not o.hide_render and
              any(m.type == "SURFACE_DEFORM" for m in o.modifiers)]
    # the vertices each cloth bone moves most, at rest (world): by the weights
    # of the mesh, or of the skeleton's mesh of the same vertices (the .blend's
    # cloth meshes may follow their cages only)
    sets, wo_name = {}, {}

    def same_mesh(o):
        for so in skel_meshes:
            if len(so.data.vertices) != len(o.data.vertices):
                continue
            a = np.empty(len(o.data.vertices) * 3)
            b = np.empty(len(o.data.vertices) * 3)
            o.data.vertices.foreach_get("co", a)
            so.data.vertices.foreach_get("co", b)
            ma, mb = np.array(o.matrix_world), np.array(so.matrix_world)
            wa = a.reshape(-1, 3) @ ma[:3, :3].T + ma[:3, 3]
            wb = b.reshape(-1, 3) @ mb[:3, :3].T + mb[:3, 3]
            if np.abs(wa - wb).max() < 0.002:
                return so
        return None
    skel_of_mesh = {}
    for o in meshes:
        wo = o
        cand = [g.name for g in o.vertex_groups if any(c in g.name for c in args.cloth) and
                g.name in rig.data.bones]
        so = same_mesh(o)
        if so is not None:
            skel_of_mesh[o.name] = so
            scand = [g.name for g in so.vertex_groups if any(c in g.name for c in args.cloth)]
            if len(scand) > len(cand):
                wo, cand = so, scand
        print("blendcloth: mesh %s: %d cloth groups%s" % (o.name, len(cand),
              " (of %s)" % so.name if wo is not o else ""))
        if not cand:
            continue
        gname = {g.index: g.name for g in wo.vertex_groups}
        idx = {n: [] for n in cand}
        # the largest weight of a bone (not of a mask group such as one of a
        # modifier)
        bones = rig.data.bones if wo is o else skel.data.bones
        isbone = {i for i, n in gname.items() if n in bones}
        for v in wo.data.vertices:
            gs = [e for e in v.groups if e.group in isbone]
            if gs:
                g = max(gs, key=lambda e: e.weight)
                n = gname.get(g.group)
                if n in idx and g.weight > 0.3:
                    idx[n].append(v.index)
        co = np.empty(len(o.data.vertices) * 3)
        o.data.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        mw = np.array(o.matrix_world)
        restw = co @ mw[:3, :3].T + mw[:3, 3]
        for n, ii in idx.items():
            if len(ii) >= 4 and n not in sets:
                sets[n] = (o, np.array(ii), restw[np.array(ii)])
                wo_name[n] = "skel" if wo is not o else "rig"
    print("blendcloth: %d cloth bones fitted from %s" %
          (len(sets), ", ".join(sorted({s[0].name for s in sets.values()}))))
    # each mesh's vertices that a cloth bone of it moves: their bones and
    # weights (summing to 1) as the skeleton's mesh of the same vertices has
    # them (what is skinned in the end; the .blend's cloth meshes may follow
    # their cages only), and the rest positions (world). A mesh without one:
    # its bones fitted each on its own.
    skin = {}
    for o in {s[0] for s in sets.values()}:
        so = skel_of_mesh.get(o.name)
        if so is None:
            continue
        mine = {n for n, s in sets.items() if s[0] is o}
        cloth = {(n if wo_name.get(n) == "skel" else rig_of.get(n, n)) for n in mine}
        gname = {g.index: g.name for g in so.vertex_groups}
        vi, vb, vw = [], [], []
        for v in so.data.vertices:
            gs = [(gname[g.group], g.weight) for g in v.groups
                  if g.weight > 0 and g.group in gname and gname[g.group] in rest]
            if not any(n in cloth for n, _ in gs):
                continue
            vi.append(v.index)
            vb.append([n for n, _ in gs])
            vw.append([w for _, w in gs])
        names = sorted({n for b in vb for n in b})
        col = {n: j for j, n in enumerate(names)}
        Wt = np.zeros((len(vi), len(names)))
        for k, (b, w) in enumerate(zip(vb, vw)):
            for n, x in zip(b, w):
                Wt[k, col[n]] += x
        Wt /= Wt.sum(1, keepdims=True)
        co = np.empty(len(o.data.vertices) * 3)
        o.data.vertices.foreach_get("co", co)
        mw = np.array(o.matrix_world)
        r = (co.reshape(-1, 3) @ mw[:3, :3].T + mw[:3, 3])[vi]
        # each cloth bone's own vertices (those it moves most): rows of vi
        row = {v: k for k, v in enumerate(vi)}
        own = {}
        for n in mine:
            sk = n if wo_name.get(n) == "skel" else rig_of.get(n, n)
            ks = [row[v] for v in sets[n][1] if v in row]
            if sk in col and ks:
                own[col[sk]] = np.array(ks)
        skin[o.name] = (np.array(vi), names, Wt, r, [col[n] for n in names if n in cloth], own)
    errs = {"each": [], "together": []}
    fitted = []
    first = nframe - 2 * args.cycle
    before = {}  # each mesh's bones' moves of the vertices, the frame before
    for f in range(1, nframe + 1):
        sc.frame_set(f)
        if f <= first:
            continue
        dg = bpy.context.evaluated_depsgraph_get()
        cache = {}
        W = dict(frames[(f - 1) % args.cycle])
        for n, (o, ii, r) in sets.items():
            if o.name not in cache:
                eo = o.evaluated_get(dg)
                me = eo.to_mesh()
                co = np.empty(len(me.vertices) * 3)
                me.vertices.foreach_get("co", co)
                mw = np.array(eo.matrix_world)
                cache[o.name] = co.reshape(-1, 3) @ mw[:3, :3].T + mw[:3, 3]
                eo.to_mesh_clear()
            e = cache[o.name][ii]
            S = Matrix.Identity(4)
            R, tr = kabsch(r, e, np.ones(len(r)))
            for i in range(3):
                for j in range(3):
                    S[i][j] = R[i][j]
                S[i][3] = tr[i]
            sk = n if wo_name.get(n) == "skel" else rig_of.get(n, n)
            if sk in rest:
                W[sk] = S @ rest[sk]
        # the cloth bones of each mesh together: the skinning fitted
        for oname, (vi, names, Wt, r, cloth, own) in skin.items():
            e = cache[oname][vi]
            # each bone's move of every vertex (B x V x 3), and all together
            M = np.array([np.array(W[n] @ rest[n].inverted()) for n in names])
            P = np.einsum("bij,vj->bvi", M[:, :3, :3], r) + M[:, None, :3, 3]
            tot = np.einsum("vb,bvi->vi", Wt, P)
            errs["each"].append(np.linalg.norm(tot - e, axis=1))
            for _ in range(args.iters):
                for j in cloth:
                    ks = Wt[:, j] > 0.02
                    w = Wt[ks, j]
                    # where bone j should take them: the simulation less the
                    # other bones' part, over its weight
                    t = (e[ks] - tot[ks] + w[:, None] * P[j, ks]) / w[:, None]
                    rr, ww = r[ks], w * w
                    if j in own:
                        o_ = own[j]
                        k_ = ww.sum() / len(o_)
                        rr, t = np.concatenate([rr, r[o_]]), np.concatenate([t, e[o_]])
                        ww = np.concatenate([ww, np.full(len(o_), args.anchor * k_)])
                        if oname in before:
                            rr = np.concatenate([rr, r[o_]])
                            t = np.concatenate([t, before[oname][j, o_]])
                            ww = np.concatenate([ww, np.full(len(o_), args.smooth * k_)])
                    R, tr = kabsch(rr, t, ww)
                    new = r @ R.T + tr
                    tot += Wt[:, j:j + 1] * (new - P[j])
                    P[j] = new
                    M[j] = np.eye(4)
                    M[j][:3, :3], M[j][:3, 3] = R, tr
            before[oname] = P.copy()
            if args.iters and cloth:
                errs["together"].append(np.linalg.norm(tot - e, axis=1))
                for j in cloth:
                    W[names[j]] = Matrix(M[j].tolist()) @ rest[names[j]]
        fitted.append(W)
        print("blendcloth: frame %d" % f) if f % 10 == 0 else None
    # the last cycle; its last frames blended into the cycle before, whose
    # last frame its first follows
    prev, last = fitted[:args.cycle], fitted[args.cycle:]
    clothb = sorted({n for sk in skin.values() for n in (sk[1][j] for j in sk[4])})
    out = []
    for i in range(args.cycle):
        W = dict(last[i])
        a = (i - (args.cycle - args.blend) + 1) / args.blend if args.blend else 0
        if a > 0:
            for n in clothb:
                l0, r0, s0 = last[i][n].decompose()
                l1, r1, s1 = prev[i][n].decompose()
                W[n] = Matrix.LocRotScale(l0.lerp(l1, a), r0.slerp(r1, a), s0.lerp(s1, a))
        out.append(W)
    # single frames of a cloth bone far from both neighbours that are close
    # to each other (a thin strip of vertices leaves its turn about its long
    # axis to chance): the neighbours' mean
    def turn(a_, b_):
        ang = np.degrees(a_.to_quaternion().rotation_difference(b_.to_quaternion()).angle)
        return min(ang, 360 - ang)
    nfix = 0
    for n in clothb:
        for _ in range(3):
            fixed = False
            for i in range(args.cycle):
                p_, c_, x_ = (out[(i - 1) % args.cycle][n], out[i][n],
                              out[(i + 1) % args.cycle][n])
                if turn(p_, x_) < args.spike and min(turn(p_, c_), turn(c_, x_)) > args.spike:
                    l0, r0, s0 = p_.decompose()
                    l1, r1, s1 = x_.decompose()
                    out[i][n] = Matrix.LocRotScale(l0.lerp(l1, 0.5), r0.slerp(r1, 0.5),
                                                   s0.lerp(s1, 0.5))
                    fixed, nfix = True, nfix + 1
            if not fixed:
                break
    print("blendcloth: %d single-frame turns of cloth bones smoothed" % nfix)
    # the cloth bones' turns between frames (the loop's seam too)
    turns = []
    for n in clothb:
        for i in range(args.cycle):
            q0 = out[i][n].to_quaternion()
            q1 = out[(i + 1) % args.cycle][n].to_quaternion()
            ang = np.degrees(q0.rotation_difference(q1).angle)
            turns.append(min(ang, 360 - ang))
    if turns:
        print("blendcloth: the cloth bones' turns between frames: median %.1f deg, 99%% %.1f, "
              "max %.1f" % (np.median(turns), np.percentile(turns, 99), max(turns)))
    out_frames = [{k: mat(v) for k, v in W.items()} for W in out]
    mm = 1000
    for k, v in errs.items():
        if v:
            a = np.concatenate(v)
            print("blendcloth: the cloth vertices' distance to the simulation, the bones fitted "
                  "%s: max %.1f mm, mean %.2f, 95%% %.2f" % (k, a.max() * mm, a.mean() * mm,
                                                           np.percentile(a, 95) * mm))
    json.dump({"armature": skel.name, "rest": {k: mat(v) for k, v in rest.items()},
               "poses": [{"name": args.name, "fps": sc.render.fps / sc.render.fps_base,
                          "frames": out_frames}]}, open(args.output, "w"))
    print("blendcloth: %s: %d frames of %s at %g fps" %
          (args.output, len(out_frames), args.name, sc.render.fps / sc.render.fps_base))


main()
