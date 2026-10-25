#!/usr/bin/env python3
"""glTF 2.0 (.glb or .gltf) to an MXM model for maxsdk (demos/model/mxm.h).

    gltf2mxm.py in.glb out.mxm --limits a_ix.h [b_ix.h ...] [options]

The model is made for VU1 programs built from HLSL (opt -vu-indexed): it is
split into clusters of at most MAX_N vertices and MAX_I strip indices (the
smallest of the --limits headers), each drawn as one indexed batch, and the
same strips are also written as plain vertex streams, for the programs that
draw strips (--layout indexed or strips: only one of the two). Every vertex attribute is a stream in the format of the shader
inputs (demos/model/m*.hlsl):

    position V3-32, normal V4-8 (length 127), texture coordinates V2-16 (4.12),
    joints V4-8U (slots of the cluster's palette), weights V4-8U (sum 255).

A skinned model has the positions and normals twice: in the rest pose (for the
programs without skinning) and as the skin has them (mskin). Its animations
are baked: the joint matrices (joint node's world matrix times its inverse
bind matrix) at --fps frames a second. Each material has one texture (32
bits, or --tex-format t8: 256 colours of a CLUT), made to fit the GS memory
(--tex-budget), and its mean colour. Needs Python 3 and
Pillow (PIL) for the images; nothing else.
"""

import argparse
import base64
import io
import json
import math
import os
import re
import struct
import sys

MAGIC = 0x324D584D  # "MXM2"
SKINNED = 1
SINGLE = 2  # drawn once, not as copies

# sections, in the order of mxm.h
SECTIONS = ["POS", "NRM", "UV", "RPOS", "RNRM", "JNT", "WGT", "IDX", "CLUSTER",
            "SPOS", "SNRM", "SUV", "SRPOS", "SRNRM", "SJNT", "SWGT", "STRIP",
            "MAT", "TEX", "PAL", "ANIM", "PHYS", "COLL"]
NSEC = 24
HEADER_BYTES = 80 + 4 * NSEC  # struct mxm_header
PAL_MAX = 30  # joints a palette record can hold


def fail(msg):
    sys.exit("gltf2mxm: " + msg)


# ---- glTF reading ------------------------------------------------------------

COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
        5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
NORM = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}


class Gltf:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.dir = os.path.dirname(os.path.abspath(path))
        self.bins = []
        if data[:4] == b"glTF":
            ver, total = struct.unpack_from("<II", data, 4)
            if ver != 2:
                fail("glTF version %d, not 2" % ver)
            at, glb_bin = 12, None
            while at < total:
                n, kind = struct.unpack_from("<II", data, at)
                chunk = data[at + 8:at + 8 + n]
                if kind == 0x4E4F534A:
                    self.g = json.loads(chunk)
                elif kind == 0x004E4942:
                    glb_bin = chunk
                at += 8 + n
            for i, b in enumerate(self.g.get("buffers", [])):
                self.bins.append(glb_bin if "uri" not in b else self.load_uri(b["uri"]))
        else:
            self.g = json.loads(data)
            for b in self.g.get("buffers", []):
                self.bins.append(self.load_uri(b["uri"]))

    def load_uri(self, uri):
        if uri.startswith("data:"):
            return base64.b64decode(uri.split(",", 1)[1])
        return open(os.path.join(self.dir, uri), "rb").read()

    def view(self, i):
        bv = self.g["bufferViews"][i]
        b = self.bins[bv["buffer"]]
        off = bv.get("byteOffset", 0)
        return b[off:off + bv["byteLength"]], bv.get("byteStride")

    def accessor(self, i):
        """The elements of accessor i: tuples of floats or ints."""
        a = self.g["accessors"][i]
        if "sparse" in a:
            fail("sparse accessors are not supported")
        fmt, size = COMP[a["componentType"]]
        nc = NCOMP[a["type"]]
        count = a["count"]
        if "bufferView" not in a:
            return [tuple([0] * nc)] * count
        data, stride = self.view(a["bufferView"])
        off = a.get("byteOffset", 0)
        stride = stride or size * nc
        st = struct.Struct("<" + fmt * nc)
        out = [st.unpack_from(data, off + k * stride) for k in range(count)]
        if a.get("normalized") and a["componentType"] in NORM:
            n = NORM[a["componentType"]]
            out = [tuple(max(c / n, -1.0) for c in e) for e in out]
        return out

    def image(self, i):
        """The bytes of image i."""
        im = self.g["images"][i]
        if "bufferView" in im:
            return self.view(im["bufferView"])[0]
        return self.load_uri(im["uri"])


# ---- small math (4x4 row-major, column vectors) --------------------------------

def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mat_vec(m, v, w=1.0):
    return [m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] + m[r][3] * w for r in range(3)]


def identity():
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def from_gltf_matrix(m):
    return [[m[c * 4 + r] for c in range(4)] for r in range(4)]


def trs(t, r, s):
    x, y, z, w = r
    rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
           [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
           [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    m = identity()
    for i in range(3):
        for j in range(3):
            m[i][j] = rot[i][j] * s[j]
        m[i][3] = t[i]
    return m


def inverse3(m):
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-20:
        return [[1.0 if r == c else 0.0 for c in range(3)] for r in range(3)]
    inv = [[0.0] * 3 for _ in range(3)]
    for r in range(3):
        for c in range(3):
            r1, r2 = [i for i in range(3) if i != c]
            c1, c2 = [i for i in range(3) if i != r]
            minor = a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]
            inv[r][c] = (-1) ** (r + c) * minor / det
    return inv


def normal_matrix(m):
    """The inverse transpose of the 3x3 part."""
    inv = inverse3(m)
    return [[inv[c][r] for c in range(3)] for r in range(3)]


def mat3_vec(m, v):
    return [m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] for r in range(3)]


def normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v] if n > 1e-20 else [0.0, 1.0, 0.0]


def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = [-x for x in b], -d
    if d > 0.9995:
        # nearly the same: the normalized lerp
        q = [x + (y - x) * t for x, y in zip(a, b)]
        n = math.sqrt(sum(c * c for c in q))
        return [c / n for c in q]
    th = math.acos(d)
    s = math.sin(th)
    wa, wb = math.sin((1 - t) * th) / s, math.sin(t * th) / s
    return [wa * x + wb * y for x, y in zip(a, b)]


# ---- the scene -----------------------------------------------------------------

class Scene:
    def __init__(self, gl):
        self.gl = gl
        g = gl.g
        self.nodes = g.get("nodes", [])
        self.parent = [-1] * len(self.nodes)
        for i, n in enumerate(self.nodes):
            for c in n.get("children", []):
                self.parent[c] = i
        scene = g.get("scenes", [{"nodes": [i for i in range(len(self.nodes))
                                            if self.parent[i] < 0]}])[g.get("scene", 0)]
        self.roots = scene["nodes"]
        self.rest = [self.local_trs(i) for i in range(len(self.nodes))]

    def local_trs(self, i):
        n = self.nodes[i]
        if "matrix" in n:
            return ("m", from_gltf_matrix(n["matrix"]))
        return ("trs", list(n.get("translation", [0, 0, 0])), list(n.get("rotation", [0, 0, 0, 1])),
                list(n.get("scale", [1, 1, 1])))

    def worlds(self, pose):
        """World matrices of all nodes for a pose (a local TRS per node)."""
        out = [None] * len(self.nodes)

        def walk(i, pm):
            p = pose[i]
            local = p[1] if p[0] == "m" else trs(p[1], p[2], p[3])
            out[i] = mat_mul(pm, local)
            for c in self.nodes[i].get("children", []):
                walk(c, out[i])
        for r in self.roots:
            walk(r, identity())
        return out

    def mesh_nodes(self):
        out = []

        def walk(i):
            if "mesh" in self.nodes[i]:
                out.append(i)
            for c in self.nodes[i].get("children", []):
                walk(c)
        for r in self.roots:
            walk(r)
        return out


# ---- primitives to triangles -------------------------------------------------------

def triangles_of(gl, prim, n):
    mode = prim.get("mode", 4)
    idx = [e[0] for e in gl.accessor(prim["indices"])] if "indices" in prim else list(range(n))
    if mode == 4:
        return [tuple(idx[k:k + 3]) for k in range(0, len(idx) - 2, 3)]
    if mode == 5:
        return [(idx[k], idx[k + 1], idx[k + 2]) if k % 2 == 0 else (idx[k + 1], idx[k], idx[k + 2])
                for k in range(len(idx) - 2)]
    if mode == 6:
        return [(idx[0], idx[k], idx[k + 1]) for k in range(1, len(idx) - 1)]
    fail("primitive mode %d is not triangles" % mode)


def quant_weights(w):
    t = sum(w)
    if t <= 0:
        return [255, 0, 0, 0]
    q = [int(round(x / t * 255)) for x in w]
    q[q.index(max(q))] += 255 - sum(q)
    return q


def quant_normal(n):
    n = normalize(n)
    return tuple(max(-127, min(127, int(round(c * 127)))) for c in n) + (0,)


def quant_uv(uv, what):
    out = []
    for c in uv:
        v = int(round(c * 4096))
        if not -32768 <= v <= 32767:
            fail("%s: texture coordinate %g is outside -8 .. 8 (V2-16 4.12)" % (what, c))
        out.append(v)
    return tuple(out)


# ---- strips --------------------------------------------------------------------

def stripify(tris):
    """Triangles (local vertex numbers) as one strip; strips are joined by
    repeating the last vertex of one and the first of the next (degenerate
    triangles; the GS does not cull, so the order of a triangle's corners does
    not matter)."""
    edges = {}
    for t, (a, b, c) in enumerate(tris):
        for e in ((a, b), (b, c), (c, a)):
            edges.setdefault(frozenset(e), []).append(t)
    left = set(range(len(tris)))

    def free_neighbours(t, used=()):
        a, b, c = tris[t]
        n = 0
        for e in ((a, b), (b, c), (c, a)):
            n += sum(1 for u in edges[frozenset(e)] if u != t and u in left and u not in used)
        return n

    def extend(s, used):
        while True:
            e = frozenset((s[-2], s[-1]))
            nxt = [u for u in edges.get(e, []) if u in left and u not in used]
            if not nxt:
                return
            # the one with the fewest free neighbours: the others stay joined
            u = min(nxt, key=lambda u: (free_neighbours(u, used), u))
            s.append([v for v in tris[u] if v not in e][0])
            used.add(u)

    def grow(t, rot):
        # forwards from the triangle, then backwards (a strip reversed is a
        # strip of the same triangles)
        s, used = list(tris[t][rot:] + tris[t][:rot]), {t}
        extend(s, used)
        s.reverse()
        extend(s, used)
        return s, used

    strips = []
    while left:
        # the longest strip from the starts with the fewest free neighbours
        starts = sorted(left, key=lambda t: (free_neighbours(t), t))[:12]
        best = max((grow(t, r) for t in starts for r in range(3)),
                   key=lambda su: (len(su[1]), -len(su[0])))
        strips.append(best[0])
        left -= best[1]
    out = list(strips[0])
    for s in strips[1:]:
        out += [out[-1], s[0]] + s
    return out


# ---- the model ---------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--limits", nargs="+", required=True,
                    help="layout headers of the indexed programs (MAX_N, MAX_I)")
    ap.add_argument("--palette", type=int, default=24, help="joints of a palette (mskin)")
    ap.add_argument("--fps", type=float, default=60.0, help="frames of the baked animations")
    ap.add_argument("--tex-budget", type=int, default=256 * 1024,
                    help="bytes of GS memory of the textures (whole pages)")
    ap.add_argument("--tex-max", type=int, default=256, help="largest texture side")
    ap.add_argument("--tex-format", choices=("ct32", "t8"), default="ct32",
                    help="textures of 32 bits, or of 8 (256 colours of a CLUT)")
    ap.add_argument("--layout", choices=("both", "indexed", "strips"), default="both",
                    help="the sections written: the clusters, the strips, or both")
    ap.add_argument("--phys", action="append", default=[], metavar="NAME=K,D[,G[,LIMIT[,R]]]",
                    help="joints whose name contains NAME move by springs: stiffness K, "
                         "damping D, gravity G, at most LIMIT radians from the animation, "
                         "R thick (kept that far out of the capsules)")
    ap.add_argument("--phys-len", type=float, default=0.05,
                    help="the length of a spring joint without a child joint")
    ap.add_argument("--collide", nargs="+", default=[], metavar="JOINT",
                    help="joints (exact names) with a capsule the spring joints stay out of, "
                         "fitted to the vertices they (and their joints below, down to the "
                         "next of these or a spring joint) move most")
    ap.add_argument("--single", action="store_true",
                    help="the model is drawn once, not as copies (a big model)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    max_nv = max_ni = None
    for h in args.limits:
        t = open(h).read()
        n = re.search(r"_MAX_N (\d+)", t)
        i = re.search(r"_MAX_I (\d+)", t)
        if not n or not i:
            fail("%s: no MAX_N and MAX_I (not an indexed program)" % h)
        max_nv = min(max_nv or 1 << 30, int(n.group(1)))
        max_ni = min(max_ni or 1 << 30, int(i.group(1)))
    if args.palette > PAL_MAX:
        fail("a palette has at most %d joints" % PAL_MAX)

    gl = Gltf(args.input)
    g = gl.g
    sc = Scene(gl)
    rest_world = sc.worlds(sc.rest)
    skins = {sc.nodes[i]["skin"] for i in sc.mesh_nodes() if "skin" in sc.nodes[i]}
    if len(skins) > 1:
        fail("more than one skin")
    skin = g["skins"][skins.pop()] if skins else None
    if skin:
        joints = skin["joints"]
        ibm = ([from_gltf_matrix(m) for m in gl.accessor(skin["inverseBindMatrices"])]
               if "inverseBindMatrices" in skin else [identity() for _ in joints])
        jrest = [mat_mul(rest_world[j], ibm[k]) for k, j in enumerate(joints)]
    else:
        joints, ibm, jrest = [], [], []
    if len(joints) > 65536:
        fail("%d joints: a palette holds joint numbers of 16 bits" % len(joints))

    # ---- vertices and triangles, welded on their final (quantized) values
    verts, vindex, tris = [], {}, []  # tris: (a, b, c, material)
    nin = 0  # vertices of the primitives
    mats_used = {}
    for ni in sc.mesh_nodes():
        node = sc.nodes[ni]
        skinned = "skin" in node
        if skin and not skinned:
            fail("node %d has a mesh without the skin of the others" % ni)
        world = rest_world[ni]
        nm = normal_matrix(world)
        for pi, prim in enumerate(g["meshes"][node["mesh"]]["primitives"]):
            at = prim["attributes"]
            pos = gl.accessor(at["POSITION"])
            n = len(pos)
            nrm = gl.accessor(at["NORMAL"]) if "NORMAL" in at else None
            uv = gl.accessor(at["TEXCOORD_0"]) if "TEXCOORD_0" in at else [(0.0, 0.0)] * n
            if skinned:
                jj = gl.accessor(at["JOINTS_0"])
                ww = gl.accessor(at["WEIGHTS_0"])
            ptris = [t for t in triangles_of(gl, prim, n) if len(set(t)) == 3]
            if nrm is None:
                # smooth normals by position, area weighted
                acc = {}
                for a, b, c in ptris:
                    pa, pb, pc = pos[a], pos[b], pos[c]
                    u = [pb[k] - pa[k] for k in range(3)]
                    v = [pc[k] - pa[k] for k in range(3)]
                    f = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
                    for x in (a, b, c):
                        s = acc.setdefault(tuple(pos[x]), [0.0, 0.0, 0.0])
                        for k in range(3):
                            s[k] += f[k]
                nrm = [tuple(normalize(acc.get(tuple(p), [0, 1, 0]))) for p in pos]
            mat = prim.get("material", -1)
            mats_used[mat] = True
            nin += n
            local = []
            for k in range(n):
                p, nn = list(pos[k]), list(nrm[k])
                if skinned:
                    w = quant_weights(list(ww[k]))
                    j = [jj[k][c] if w[c] else jj[k][0] for c in range(4)]
                    # the rest pose: the skin's joint matrices at rest
                    sp, sn = [0.0] * 3, [0.0] * 3
                    for c in range(4):
                        if w[c]:
                            m = jrest[j[c]]
                            q, r = mat_vec(m, p), mat3_vec(m, nn)
                            for d in range(3):
                                sp[d] += q[d] * w[c] / 255
                                sn[d] += r[d] * w[c] / 255
                    key = (tuple(sp), quant_normal(sn), quant_uv(uv[k], "mesh %d" % node["mesh"]),
                           tuple(p), quant_normal(nn), tuple(j), tuple(w), mat)
                else:
                    key = (tuple(mat_vec(world, p)), quant_normal(mat3_vec(nm, nn)),
                           quant_uv(uv[k], "mesh %d" % node["mesh"]), None, None, None, None, mat)
                v = vindex.get(key)
                if v is None:
                    v = vindex[key] = len(verts)
                    verts.append(key)
                local.append(v)
            for a, b, c in ptris:
                t = (local[a], local[b], local[c])
                if len(set(t)) == 3:
                    tris.append(t + (mat,))
    if not tris:
        fail("no triangles")
    njoint = len(joints)

    # bounds of the rest pose
    lo = [min(v[0][k] for v in verts) for k in range(3)]
    hi = [max(v[0][k] for v in verts) for k in range(3)]
    center = [(lo[k] + hi[k]) / 2 for k in range(3)]
    radius = max(math.sqrt(sum((v[0][k] - center[k]) ** 2 for k in range(3))) for v in verts)

    # ---- materials: an index each, and their texture
    mat_ids = sorted(mats_used)
    mat_of = {m: i for i, m in enumerate(mat_ids)}

    # ---- clusters
    def joints_of(v):
        return set(verts[v][5]) if skin else set()

    clusters = []  # (tris as tuples of vertex ids, material)
    for m in mat_ids:
        mt = [t[:3] for t in tris if t[3] == m]
        # spatial order of the seeds: the Morton code of the centroid
        def morton(t):
            c = [sum(verts[v][0][k] for v in t) / 3 for k in range(3)]
            q = [min(1023, max(0, int((c[k] - lo[k]) / (hi[k] - lo[k] + 1e-9) * 1023))) for k in range(3)]
            code = 0
            for bit in range(10):
                for k in range(3):
                    code |= (q[k] >> bit & 1) << (3 * bit + k)
            return code
        order = sorted(range(len(mt)), key=lambda i: morton(mt[i]))
        by_vert = {}
        for i, t in enumerate(mt):
            for v in t:
                by_vert.setdefault(v, []).append(i)
        done = [False] * len(mt)
        oi = 0
        while True:
            while oi < len(order) and done[order[oi]]:
                oi += 1
            if oi == len(order):
                break
            ct, cv, cj = [], set(), set()

            def fits(i):
                nv = cv | set(mt[i])
                if len(nv) > max_nv:
                    return False
                if skin:
                    nj = set(cj)
                    for v in mt[i]:
                        nj |= joints_of(v)
                    if len(nj) > args.palette:
                        return False
                return True

            def add(i):
                ct.append(i)
                done[i] = True
                cv.update(mt[i])
                for v in mt[i]:
                    cj.update(joints_of(v))

            def strip_len(ts):
                vs = {}
                for i in ts:
                    for v in mt[i]:
                        vs.setdefault(v, len(vs))
                return len(stripify([tuple(vs[v] for v in mt[i]) for i in ts]))

            seed = order[oi]
            if not fits(seed):
                fail("a triangle has more than %d joints" % args.palette)
            add(seed)
            while True:
                cand = {}
                for v in cv:
                    for i in by_vert.get(v, []):
                        if not done[i]:
                            cand[i] = sum(1 for x in mt[i] if x not in cv)
                if not cand:
                    break
                ok = [i for i in sorted(cand, key=lambda i: (cand[i], i)) if fits(i)]
                added = False
                for i in ok[:8]:
                    # the strip of the cluster with it must fit (worst case
                    # 5 indices per triangle: below that, no need to look)
                    if 5 * (len(ct) + 1) > max_ni and strip_len(ct + [i]) > max_ni:
                        continue
                    add(i)
                    added = True
                    break
                if not added:
                    break
            clusters.append((ct, m))
    # ---- palettes: one when the skeleton fits, else per cluster, merged
    pal_of = [0] * len(clusters)
    palettes = []
    if skin:
        if njoint <= args.palette:
            palettes = [list(range(njoint))]
        else:
            cur, cur_m = None, None
            for ci, (ct, m) in enumerate(clusters):
                mt = [t[:3] for t in tris if t[3] == m]
                js = set()
                for i in ct:
                    for v in mt[i]:
                        js |= joints_of(v)
                if cur is not None and cur_m == m and len(cur | js) <= args.palette:
                    cur |= js
                else:
                    if cur is not None:
                        palettes.append(sorted(cur))
                    cur, cur_m = set(js), m
                pal_of[ci] = len(palettes)
            palettes.append(sorted(cur))
    slot = [{j: s for s, j in enumerate(p)} for p in palettes]

    # ---- streams
    S = {k: bytearray() for k in SECTIONS}
    stream_n = [0]  # vertices in the cluster streams

    def emit(dst, vs, pal):
        """The vertices vs into the streams of dst ("" or "S")."""
        for v in vs:
            k = verts[v]
            S[dst + "POS"] += struct.pack("<3f", *k[0])
            S[dst + "NRM"] += struct.pack("<4b", *k[1])
            S[dst + "UV"] += struct.pack("<2h", *k[2])
            if skin:
                S[dst + "RPOS"] += struct.pack("<3f", *k[3])
                S[dst + "RNRM"] += struct.pack("<4b", *k[4])
                S[dst + "JNT"] += struct.pack("<4B", *[slot[pal][j] for j in k[5]])
                S[dst + "WGT"] += struct.pack("<4B", *k[6])

    cluster_recs, strips_by_group = [], {}
    ntri = 0
    for ci, (ct, m) in enumerate(clusters):
        mt = [t[:3] for t in tris if t[3] == m]
        vs = {}
        for i in ct:
            for v in mt[i]:
                vs.setdefault(v, len(vs))
        strip = stripify([tuple(vs[v] for v in mt[i]) for i in ct])
        if len(strip) > max_ni or len(vs) > max_nv:
            fail("internal: a cluster of %d vertices, %d indices" % (len(vs), len(strip)))
        ntri += len(ct)
        # the vertices in the order of their first index
        first_use = []
        seen = set()
        for x in strip:
            if x not in seen:
                seen.add(x)
                first_use.append(x)
        renum = {x: k for k, x in enumerate(first_use)}
        inv = {k: v for v, k in vs.items()}
        order = [inv[x] for x in first_use]
        first = stream_n[0]
        emit("", order + [order[-1]] * ((-len(order)) % 4), pal_of[ci])
        stream_n[0] += len(order) + (-len(order)) % 4
        idx_at = len(S["IDX"])
        S["IDX"] += bytes(renum[x] for x in strip) + bytes((-len(strip)) % 16)
        pts = [verts[v][0] for v in order]
        cc = [sum(p[k] for p in pts) / len(pts) for k in range(3)]
        cr = max(math.sqrt(sum((p[k] - cc[k]) ** 2 for k in range(3))) for p in pts)
        cluster_recs.append(struct.pack("<IHHIHH4f", first, len(order), len(strip), idx_at,
                                        mat_of[m], pal_of[ci], cc[0], cc[1], cc[2], cr))
        strips_by_group.setdefault((mat_of[m], pal_of[ci]), []).append([order[x] for x in
                                                                         [renum[y] for y in strip]])
    # 3 more vertices after the last: a batch reads up to ALIGN - 1 more (the
    # last one again: its joints are in the last cluster's palette)
    if clusters:
        emit("", [order[-1]] * 4, pal_of[-1])
    S["CLUSTER"] = bytearray(b"".join(cluster_recs))

    # the strip path: each (material, palette) one strip of its clusters'
    strip_recs, sn = [], 0
    for (mi, pi), lst in sorted(strips_by_group.items()):
        seq = list(lst[0])
        for s in lst[1:]:
            seq += [seq[-1], s[0]] + s
        emit("S", seq + [seq[-1]] * (((-len(seq)) % 4) + 4), pi)
        strip_recs.append(struct.pack("<IIHHI", sn, len(seq), mi, pi, 0))
        sn += len(seq) + ((-len(seq)) % 4) + 4
    S["STRIP"] = bytearray(b"".join(strip_recs))

    # ---- textures
    from PIL import Image
    imgs = []
    for m in mat_ids:
        mat = g["materials"][m] if m >= 0 else {}
        pbr = mat.get("pbrMetallicRoughness", {})
        fac = pbr.get("baseColorFactor", [1, 1, 1, 1])
        im = None
        if "baseColorTexture" in pbr:
            tex = g["textures"][pbr["baseColorTexture"]["index"]]
            if "source" in tex:
                im = Image.open(io.BytesIO(gl.image(tex["source"]))).convert("RGBA")
        if im is None:
            im = Image.new("RGBA", (8, 8), (255, 255, 255, 255))
        imgs.append((im, fac))

    def pow2(n):
        p = 8
        while p * 2 <= n:
            p *= 2
        return p

    t8 = args.tex_format == "t8"

    def pages(w, h):
        # GS pages (8 KB) of a texture: T8 pages are 128 x 64, CT32 64 x 32
        return (-(-max(w, 128) // 128) * -(-h // 64) if t8 else
                -(-max(w, 64) // 64) * -(-h // 32))
    sizes = [[min(pow2(im.width), args.tex_max), min(pow2(im.height), args.tex_max)]
             for im, _ in imgs]
    while sum(pages(w, h) for w, h in sizes) * 8192 > args.tex_budget:
        k = max(range(len(sizes)), key=lambda i: sizes[i][0] * sizes[i][1])
        if sizes[k][0] <= 8 and sizes[k][1] <= 8:
            break
        if sizes[k][0] >= sizes[k][1]:
            sizes[k][0] //= 2
        else:
            sizes[k][1] //= 2
    # CSM1: the GS keeps entries 8 .. 15 and 16 .. 23 of each 32 swapped
    csm1 = [(i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1 for i in range(256)]
    mat_recs = []
    for mi, ((im, fac), (w, h)) in enumerate(zip(imgs, sizes)):
        # the highlight's strength: smooth and metallic, strong
        pbr = (g["materials"][mat_ids[mi]] if mat_ids[mi] >= 0 else {}).get(
            "pbrMetallicRoughness", {})
        rough, metal = pbr.get("roughnessFactor", 1.0), pbr.get("metallicFactor", 1.0)
        spec = min(255, int(255 * (1 - rough) ** 2 * (0.4 + 0.6 * metal) * 1.5))
        im = im.resize((w, h), Image.LANCZOS)
        px = im.load()
        rgba = []
        tot = [0, 0, 0]
        for y in range(h):
            for x in range(w):
                r, gg, b, a = px[x, y]
                c = [int(round(r * fac[0])), int(round(gg * fac[1])), int(round(b * fac[2])),
                     int(round(a * fac[3]))]
                for k in range(3):
                    tot[k] += c[k]
                rgba.append(c)
        mean = [int(t / (w * h)) for t in tot]
        if t8:
            src = Image.new("RGBA", (w, h))
            src.putdata([tuple(c) for c in rgba])
            q = src.quantize(256, method=Image.Quantize.FASTOCTREE,
                             dither=Image.Dither.FLOYDSTEINBERG)
            pal = q.getpalette("RGBA")[:256 * 4]
            pal += [0] * (256 * 4 - len(pal))
            clut = bytearray(1024)
            for i in range(256):
                r, gg, b, a = pal[4 * i:4 * i + 4]
                # GS alpha: 128 is 1.0
                clut[4 * csm1[i]:4 * csm1[i] + 4] = bytes([r, gg, b, (a * 128 + 127) // 255])
            S["TEX"] += bytes((-len(S["TEX"])) % 16)
            clut_off = len(S["TEX"])
            S["TEX"] += clut
            off = len(S["TEX"])
            S["TEX"] += q.tobytes()
        else:
            off, clut_off = len(S["TEX"]), 0
            S["TEX"] += bytes(v for c in rgba for v in c[:3] + [(c[3] * 128 + 127) // 255])
        # texels less than fully opaque (GS alpha 128): drawn blended, after the rest
        if t8:
            used = set(q.tobytes())
            alpha = any(pal[4 * i + 3] < 255 for i in used)
        else:
            alpha = any(c[3] < 255 for c in rgba)
        mat_recs.append(struct.pack("<IBBBBBBBBI", off, int(math.log2(w)), int(math.log2(h)),
                                    1 if t8 else 0, 1 if alpha else 0, mean[0], mean[1], mean[2],
                                    spec, clut_off))
    S["MAT"] = bytearray(b"".join(mat_recs))

    # ---- palettes and animations
    for p in palettes:
        S["PAL"] += struct.pack("<H%dHH" % PAL_MAX, len(p), *(p + [0] * (PAL_MAX - len(p))), 0)
    anims = []
    if skin:
        def frame_mats(world):
            out = []
            for k, j in enumerate(joints):
                m = mat_mul(world[j], ibm[k])
                for c in range(4):
                    out.append([m[0][c], m[1][c], m[2][c], m[3][c]])
            return out
        anims.append(("Rest", [frame_mats(rest_world)]))
        for a in g.get("animations", []):
            chans = []
            for ch in a["channels"]:
                t = ch["target"]
                if t.get("path") not in ("translation", "rotation", "scale") or "node" not in t:
                    continue
                smp = a["samplers"][ch["sampler"]]
                times = [e[0] for e in gl.accessor(smp["input"])]
                vals = gl.accessor(smp["output"])
                chans.append((t["node"], t["path"], smp.get("interpolation", "LINEAR"), times, vals))
            if not chans:
                continue
            dur = max(c[3][-1] for c in chans)
            nf = max(1, int(round(dur * args.fps)))
            frames = []
            for f in range(nf):
                tm = f / args.fps
                pose = list(sc.rest)
                pose = [list(p) if p[0] == "trs" else p for p in pose]
                for node, path, interp, times, vals in chans:
                    if pose[node][0] != "trs":
                        pose[node] = ["trs", [0, 0, 0], [0, 0, 0, 1], [1, 1, 1]]
                    cubic = interp == "CUBICSPLINE"
                    k = 0
                    while k + 1 < len(times) and times[k + 1] <= tm:
                        k += 1
                    def val(i):
                        return list(vals[3 * i + 1] if cubic else vals[i])
                    if tm <= times[0] or len(times) == 1:
                        v = val(0)
                    elif k + 1 >= len(times):
                        v = val(len(times) - 1)
                    else:
                        t0, t1 = times[k], times[k + 1]
                        u = (tm - t0) / (t1 - t0)
                        if interp == "STEP":
                            v = val(k)
                        elif cubic:
                            d = t1 - t0
                            p0, m0 = val(k), list(vals[3 * k + 2])
                            p1, m1 = val(k + 1), list(vals[3 * (k + 1)])
                            u2, u3 = u * u, u * u * u
                            v = [(2 * u3 - 3 * u2 + 1) * p0[c] + (u3 - 2 * u2 + u) * d * m0[c] +
                                 (-2 * u3 + 3 * u2) * p1[c] + (u3 - u2) * d * m1[c]
                                 for c in range(len(p0))]
                            if path == "rotation":
                                v = normalize(v)
                        elif path == "rotation":
                            v = slerp(val(k), val(k + 1), u)
                        else:
                            v = [x + (y - x) * u for x, y in zip(val(k), val(k + 1))]
                    slot_i = {"translation": 1, "rotation": 2, "scale": 3}[path]
                    pose[node][slot_i] = v
                frames.append(frame_mats(sc.worlds([tuple(p) for p in pose])))
            anims.append((a.get("name") or "anim%d" % len(anims), frames))
        table = bytearray()
        data = bytearray()
        base = 32 * len(anims)
        for name, frames in anims:
            nm = name.encode()[:15]
            table += struct.pack("<16sIfII", nm + bytes(16 - len(nm)), len(frames), args.fps,
                                 base + len(data), 0)
            for fr in frames:
                for col in fr:
                    data += struct.pack("<4f", *col)
        S["ANIM"] = table + data
        if args.verbose:
            for name, frames in anims:
                # the drift of the first joint over the animation
                a0, a1 = frames[0][3], frames[-1][3]
                print("gltf2mxm: anim %s: %d frames, root moves %.2f" %
                      (name, len(frames), math.sqrt(sum((a1[k] - a0[k]) ** 2 for k in range(3)))))

    # ---- the spring joints, parents before children
    if skin and args.phys:
        specs = []
        for spec in args.phys:
            name, vals = spec.split("=", 1)
            v = [float(x) for x in vals.split(",")]
            specs.append((name, (v + [0.0, 0.6, 0.01][len(v) - 2:])[:5]))
        jnum = {node: k for k, node in enumerate(joints)}

        def depth(i):
            d = 0
            while sc.parent[i] >= 0:
                i, d = sc.parent[i], d + 1
            return d

        def below(i):
            out = []
            for c in sc.nodes[i].get("children", []):
                if c in jnum:
                    out.append(jnum[c])
                out += below(c)
            return out
        recs, descs = [], []
        for node in sorted(joints, key=depth):
            nm = sc.nodes[node].get("name", "")
            par = next((v for k, v in specs if k in nm), None)
            if par is None:
                continue
            m = rest_world[node]
            head = [m[r][3] for r in range(3)]
            kids = [c for c in sc.nodes[node].get("children", []) if c in jnum]
            if kids:
                tip = [rest_world[kids[0]][r][3] for r in range(3)]
            else:
                y = normalize([m[r][1] for r in range(3)])
                tip = [head[r] + y[r] * args.phys_len for r in range(3)]
            d = below(node)
            recs.append((jnum[node], d, head, tip, par))
            descs += d
        base = 16 + 72 * len(recs)  # struct mxm_phys
        S["PHYS"] = bytearray(struct.pack("<4I", len(recs), 0, 0, 0))
        at = 0
        for k, d, head, tip, par in recs:
            S["PHYS"] += struct.pack("<HHI8f5f3I", k, len(d), base + 2 * at, *head, 1.0, *tip,
                                     1.0, *par, 0, 0, 0)
            at += len(d)
        S["PHYS"] += struct.pack("<%dH" % len(descs), *descs)
        print("gltf2mxm: %d spring joints, %d below them" % (len(recs), len(descs)))

    # ---- the capsules: fitted to the vertices each joint moves most (their
    # main direction, extent and distance to the axis)
    if skin and args.collide:
        names = {sc.nodes[node].get("name", ""): k for k, node in enumerate(joints)}
        springs = [spec.split("=", 1)[0] for spec in args.phys]
        coll = set(args.collide)

        def owner(k):
            # the capsule joint at or above joint k (twist joints, fingers: their
            # bone's), none past a spring joint
            i = joints[k]
            while i >= 0:
                nm = sc.nodes[i].get("name", "")
                if nm in coll:
                    return names.get(nm)
                if any(sp in nm for sp in springs):
                    return None
                i = sc.parent[i]
            return None
        own = [owner(k) for k in range(len(joints))]
        pts = {}
        for v in verts:
            j, w = v[5], v[6]
            o = own[j[w.index(max(w))]]
            if o is not None:
                pts.setdefault(o, []).append(v[0])
        recs = []
        for nm in args.collide:
            k = names.get(nm)
            ps = pts.get(k, []) if k is not None else []
            if len(ps) < 8:
                print("gltf2mxm: collide %s: no joint, or too few vertices" % nm)
                continue
            n = len(ps)
            mean = [sum(p[c] for p in ps) / n for c in range(3)]
            cov = [[sum((p[r] - mean[r]) * (p[c] - mean[c]) for p in ps) / n for c in range(3)]
                   for r in range(3)]
            # the axis: the points' main direction, or towards a capsule joint below
            # (the bone): whichever fits closer (the smaller radius; the bone up to
            # 15% wider)
            axes = []
            ax = [1.0, 1.0, 1.0]
            for _ in range(50):
                ax = normalize([sum(cov[r][c] * ax[c] for c in range(3)) for r in range(3)])
            axes.append((ax, None))
            hd = [rest_world[joints[k]][r][3] for r in range(3)]
            for c in sc.nodes[joints[k]].get("children", []):
                if sc.nodes[c].get("name", "") in coll:
                    tl = [rest_world[c][r][3] for r in range(3)]
                    axes.append((normalize([tl[r] - hd[r] for r in range(3)]), (hd, tl)))

            def fit(axis, bone):
                # the extent: the bone's (head to the joint below), else the points'
                if bone:
                    t0, t1 = sorted(sum((q[c] - mean[c]) * axis[c] for c in range(3))
                                    for q in bone)
                else:
                    ts = sorted(sum((p[c] - mean[c]) * axis[c] for c in range(3)) for p in ps)
                    t0, t1 = ts[int(n * 0.05)], ts[int(n * 0.95)]
                ds = []
                for p in ps:
                    t = min(max(sum((p[c] - mean[c]) * axis[c] for c in range(3)), t0), t1)
                    ds.append(math.sqrt(sum((p[c] - mean[c] - axis[c] * t) ** 2
                                            for c in range(3))))
                ds.sort()
                return ds[int(n * 0.8)], t0, t1, axis
            # the bone, unless the main direction fits clearly closer
            fits = [fit(ax, bone) for ax, bone in axes]
            r, t0, t1, axis = min(fits, key=lambda f: f[0] * (1.0 if f is fits[0] else 1 / 1.15))
            # the caps are part of the length
            t0, t1 = (t0 + r, t1 - r) if t1 - t0 > 2 * r else ((t0 + t1) / 2,) * 2
            a = [mean[c] + axis[c] * t0 for c in range(3)]
            b = [mean[c] + axis[c] * t1 for c in range(3)]
            recs.append(struct.pack("<HHf6f", k, 0, r, *a, *b))
            if args.verbose:
                print("gltf2mxm: collide %-12s r %.3f length %.3f (%d vertices)" %
                      (nm, r, t1 - t0, n))
        S["COLL"] = bytearray(struct.pack("<4I", len(recs), 0, 0, 0)) + b"".join(recs)
        print("gltf2mxm: %d capsules" % len(recs))

    # ---- the layout: the sections of the other one left out
    ncluster, nvert, nstrip_vert, nstrip = len(clusters), stream_n[0] + 4, sn, len(strip_recs)
    if args.layout == "strips":
        for k in ("POS", "NRM", "UV", "RPOS", "RNRM", "JNT", "WGT", "IDX", "CLUSTER"):
            S[k] = bytearray()
        ncluster = nvert = 0
    elif args.layout == "indexed":
        for k in ("SPOS", "SNRM", "SUV", "SRPOS", "SRNRM", "SJNT", "SWGT", "STRIP"):
            S[k] = bytearray()
        nstrip_vert = nstrip = 0

    # ---- the file
    out = bytearray(HEADER_BYTES)
    offs = [0] * NSEC
    for i, k in enumerate(SECTIONS):
        if not S[k]:
            continue
        out += bytes((-len(out)) % 64)
        offs[i] = len(out)
        out += S[k]
    out += bytes((-len(out)) % 64)
    flags = (SKINNED if skin else 0) | (SINGLE if args.single else 0)
    hdr = struct.pack("<16I4f", MAGIC, len(out), flags, nvert,
                      ncluster, ntri, nstrip_vert, nstrip,
                      len(mat_ids), njoint, len(palettes), len(anims),
                      max_nv, max_ni, args.palette, len(S["TEX"]),
                      center[0], center[1], center[2], radius)
    hdr += struct.pack("<%dI" % NSEC, *offs)
    assert len(hdr) == HEADER_BYTES
    out[:HEADER_BYTES] = hdr
    open(args.output, "wb").write(out)
    nidx = sum(struct.unpack_from("<IHH", r)[2] for r in cluster_recs)
    uniq = sum(struct.unpack_from("<IHH", r)[1] for r in cluster_recs)
    print("gltf2mxm: %s: %d triangles, %d vertices (%d welded), %d clusters of <= %d vertices "
          "and %d indices: %d vertices, %d indices (%.2f per triangle; strips %d vertices), "
          "%d materials, %d joints, %d palettes, %d animations, %s, %d bytes" %
          (os.path.basename(args.output), ntri, nin, len(verts), len(clusters), max_nv,
           max_ni, uniq, nidx, nidx / ntri, sn, len(mat_ids), njoint, len(palettes),
           len(anims), args.layout, len(out)))
    if args.verbose:
        for i, k in enumerate(SECTIONS):
            if S[k]:
                print("gltf2mxm: %-8s %9d bytes" % (k, len(S[k])))


if __name__ == "__main__":
    main()
