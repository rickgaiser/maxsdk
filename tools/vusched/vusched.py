#!/usr/bin/env python3
"""vusched: modulo scheduler, emitter and checker for the vumax VU1 kernels.

A kernel is the per-vertex work of a vertex program as a list of ops (one
upper or lower instruction each). vusched finds a modulo schedule for u
vertices per loop iteration at a given II, allocates the VF registers, and
writes a .vsm program for demos/vumax (layout in demos/vumax/vumax.h):

  the header at ITOP: x n, y context, z kick (0: no XGKICK), w loop end;
  the GIF tag after it, then the vertices, stride quadwords each, in place:
  the VIF unpacked the inputs into the slots that the GIF registers take.

The loop has no prologue or epilogue: it runs S - 1 kernel iterations more
than the vertices need (S stages). The ones at the start store into the pad
before the header, the header and the GIF tag (the program writes the tag
again after the loop), the ones at the end into the slop after the vertices.

Rules the schedule keeps (VU User's Manual 7.4, docs/vu1.md):
  * one upper and one lower op per word; FMAC, LQ, MFP, MOVE, MFIR results
    can be read 4 cycles after the issue;
  * a register is read before its next write issues (the VU stalls a read
    while a write to the register is in flight);
  * DIV: one per 7 cycles, Q new from issue + 7 (reads are not interlocked:
    an earlier read gets the old Q); EFU (ERCPR): one per 11 cycles, P new
    from issue + 12; clip flags: new from CLIPW issue + 4;
  * ACC chains (MULA/MADDA .. MADD) of different vertices do not interleave;
  * the branch is in word II - 2, its delay slot II - 1; the pointer
    increment is not in word II - 3 (a VI write is not seen by the next
    word's branch).

The checker (check()) runs the emitted loop on a cycle model with symbolic
values: every register lane, Q, P, ACC, the clip flags and every memory
quadword carries the op and iteration that wrote it, and every read is
compared with what the schedule meant. It reports stalls, cycles per
iteration and wrong reads.

  vusched.py [--out DIR] [kernel ...]      schedule, check and emit
"""

import argparse
import itertools
import os
import sys

LANES = "xyzw"
FIELD = {c: i for i, c in enumerate(LANES)}

DIV_BUSY, DIV_LAT = 7, 7
EFU_BUSY, EFU_LAT = 11, 12
CLIP_LAT = 4
VF_LAT = 4

# VI registers of every program (VI01 is fcand's)
VI_FC, VI_ADC = "vi01", "vi02"
VI_CTX, VI_P, VI_END, VI_KICK, VI_TAG, VI_B = "vi03", "vi04", "vi05", "vi06", "vi07", "vi08"


class Op:
    """One instruction of a vertex.

    text: the instruction, with {name} for the register of a group of this
    vertex (or a uniform), {off} for a memory offset and {p} for the pointer.
    dst: (group, lanes) written; srcs: [(group, lanes)] read. Special
    groups: Q, P (EFU result), ACC, CF (clip flags), VI:<name>.
    """

    def __init__(self, name, unit, text, dst=None, srcs=(), lat=VF_LAT, res=None,
                 mem=None):
        self.name, self.unit, self.text = name, unit, text
        self.dst = dst
        self.srcs = list(srcs)
        self.lat = lat
        self.res = res          # 'DIV', 'EFU' or None
        self.mem = mem          # ('lq'|'sq', slot)


class Kernel:
    def __init__(self, name, ops_for, u, ii, stride, uniforms, presets, notes=""):
        self.name = name
        self.u, self.ii, self.stride = u, ii, stride
        self.ops_for = ops_for    # copy -> [Op]
        self.uniforms = uniforms  # [(group, ctx qw)] loaded at the start
        self.presets = presets    # {group: (lanes, text)} lanes set before the loop
        self.notes = notes


# ---- the vertex programs ---------------------------------------------------

def xform(k, slot, recip):
    """Position: MVP, the perspective divide by DIV (recip 'div') or the
    EFU (recip 'efu'), into P (clip space) and T (P * 1/w)."""
    ops = [
        Op("lqpos", "L", "lq.xyz {V}, {off}({p})", ("V", "xyz"), mem=("lq", slot)),
        Op("mx", "U", "mulax.xyzw ACC, {C0}, {V}x", ("ACC", ""), [("V", "x")]),
        Op("my", "U", "madday.xyzw ACC, {C1}, {V}y", ("ACC", ""), [("ACC", ""), ("V", "y")]),
        Op("mz", "U", "maddaz.xyzw ACC, {C2}, {V}z", ("ACC", ""), [("ACC", ""), ("V", "z")]),
        Op("mw", "U", "maddw.xyzw {P}, {C3}, vf00w", ("P", "xyzw"), [("ACC", "")]),
    ]
    if recip == "div":
        ops += [
            Op("div", "L", "div Q, vf00w, {P}w", ("Q", ""), [("P", "w")], lat=DIV_LAT, res="DIV"),
            Op("mulq", "U", "mulq.xyz {T}, {P}, Q", ("T", "xyz"), [("P", "xyz"), ("Q", "")]),
        ]
    else:
        ops += [
            Op("rcp", "L", "ercpr P, {P}w", ("PR", ""), [("P", "w")], lat=EFU_LAT, res="EFU"),
            Op("mfp", "L", "mfp.w {R}, P", ("R", "w"), [("PR", "")]),
            Op("mulr", "U", "mulw.xyz {T}, {P}, {R}w", ("T", "xyz"), [("P", "xyz"), ("R", "w")]),
        ]
    return ops


def recip_src(recip):
    return [("Q", "")] if recip == "div" else [("R", "w")]


def recip_text(recip, d, s):
    return f"mulq.xyz {{{d}}}, {{{s}}}, Q" if recip == "div" else f"mulw.xyz {{{d}}}, {{{s}}}, {{R}}w"


def xyz_out(slot):
    """T to 12.4 fixed point into O (w preset: no ADC), stored."""
    return [
        Op("ftoi", "U", "ftoi4.xyz {O}, {T}", ("O", "xyz"), [("T", "xyz")]),
        Op("sqpos", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", slot)),
    ]


def st_ops(slot, recip):
    """ST (s, t, 1) times 1/w: S, T, Q of the GIF ST register."""
    return [
        Op("lqst", "L", "lq.xyz {S}, {off}({p})", ("S", "xyz"), mem=("lq", slot)),
        Op("mulst", "U", recip_text(recip, "S2", "S"), ("S2", "xyz"),
           [("S", "xyz")] + recip_src(recip)),
        Op("sqst", "L", "sq.xyz {S2}, {off}({p})", None, [("S2", "xyz")], mem=("sq", slot)),
    ]


def st16_ops(slot):
    """ST as V2-16 in 4.12 fixed point (z = 4096 from the VIF), ITOF12, times
    1/w: one upper op more than st_ops, a third of the VIF bytes."""
    return [
        Op("lqst", "L", "lq.xyz {S}, {off}({p})", ("S", "xyz"), mem=("lq", slot)),
        Op("itof", "U", "itof12.xyz {S1}, {S}", ("S1", "xyz"), [("S", "xyz")]),
        Op("mulst", "U", recip_text("div", "S2", "S1"), ("S2", "xyz"), [("S1", "xyz"), ("Q", "")]),
        Op("sqst", "L", "sq.xyz {S2}, {off}({p})", None, [("S2", "xyz")], mem=("sq", slot)),
    ]


def light1(slot):
    """One directional light: N . L per colour lane (D0..D2 = diffuse * L),
    clamped at 0, plus ambient + 1.5 * 2^23 (the low 8 bits of each lane are
    the colour: the GIF takes RGBA from bits 0-7). The normal is read from
    the RGBAQ slot and the colour stored over it; w (alpha) is preset."""
    return [
        Op("lqn", "L", "lq.xyz {N}, {off}({p})", ("N", "xyz"), mem=("lq", slot)),
        Op("lx", "U", "mulax.xyz ACC, {D0}, {N}x", ("ACC", ""), [("N", "x")]),
        Op("ly", "U", "madday.xyz ACC, {D1}, {N}y", ("ACC", ""), [("ACC", ""), ("N", "y")]),
        Op("lz", "U", "maddz.xyz {L}, {D2}, {N}z", ("L", "xyz"), [("ACC", ""), ("N", "z")]),
        Op("lmax", "U", "max.xyz {L2}, {L}, vf00", ("L2", "xyz"), [("L", "xyz")]),
        Op("ladd", "U", "add.xyz {K}, {L2}, {AMB}", ("K", "xyz"), [("L2", "xyz")]),
        Op("sqk", "L", "sq.xyzw {K}, {off}({p})", None, [("K", "xyzw")], mem=("sq", slot)),
    ]


def light3(slot):
    """Three directional lights (ps2gl's SCEI shader): the cosines (A0..A2:
    columns of the light directions), clamped at 0, times the light colours
    (E0..E2), plus ambient + 1.5 * 2^23, clamped at 255 (+ 1.5 * 2^23)."""
    return [
        Op("lqn", "L", "lq.xyz {N}, {off}({p})", ("N", "xyz"), mem=("lq", slot)),
        Op("cx", "U", "mulax.xyz ACC, {A0}, {N}x", ("ACC", ""), [("N", "x")]),
        Op("cy", "U", "madday.xyz ACC, {A1}, {N}y", ("ACC", ""), [("ACC", ""), ("N", "y")]),
        Op("cz", "U", "maddz.xyz {L}, {A2}, {N}z", ("L", "xyz"), [("ACC", ""), ("N", "z")]),
        Op("cmax", "U", "max.xyz {L2}, {L}, vf00", ("L2", "xyz"), [("L", "xyz")]),
        Op("ex", "U", "mulax.xyz ACC, {E0}, {L2}x", ("ACC", ""), [("L2", "x")]),
        Op("ey", "U", "madday.xyz ACC, {E1}, {L2}y", ("ACC", ""), [("ACC", ""), ("L2", "y")]),
        Op("ez", "U", "maddz.xyz {L3}, {E2}, {L2}z", ("L3", "xyz"), [("ACC", ""), ("L2", "z")]),
        Op("cadd", "U", "add.xyz {L4}, {L3}, {AMB}", ("L4", "xyz"), [("L3", "xyz")]),
        Op("cmin", "U", "mini.xyz {K}, {L4}, {MAX}", ("K", "xyz"), [("L4", "xyz")]),
        Op("sqk", "L", "sq.xyzw {K}, {off}({p})", None, [("K", "xyzw")], mem=("sq", slot)),
    ]


def scei_xyz(slot):
    """S6 position out: guard band test of T (offset-free pixels * 16, z
    centred) against GB.w by CLIPW; the ADC bit from the last three tests
    (the triangle of this vertex); T + (1.5 * 2^23 + offset) puts the 12.4
    fixed-point X, Y and Z in the low bits of each lane (FTOI4 and the
    screen offset in one op)."""
    return [
        Op("clip", "U", "clipw.xyz {T}xyz, {GB}w", ("CF", ""), [("T", "xyz")], lat=CLIP_LAT),
        Op("fc", "L", "fcand vi01, 0x3ffff", ("VI:fc", ""), [("CF", "")], lat=1),
        Op("adc", "L", "iaddiu vi02, vi01, 0x7fff", ("VI:adc", ""), [("VI:fc", "")], lat=1),
        Op("mfir", "L", "mfir.w {O}, vi02", ("O", "w"), [("VI:adc", "")]),
        Op("mag", "U", "add.xyz {O}, {T}, {MAG}", ("O", "xyz"), [("T", "xyz")]),
        Op("sqpos", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", slot)),
    ]


MVP = [("C0", 0), ("C1", 1), ("C2", 2), ("C3", 3)]
O_PRESET = {"O": ("w", "move.w {O}, vf00")}  # w = 1.0: bit 15 (ADC) clear


def k_flat(name, stride):
    """S1 (stride 1: XYZ2) and S2 (stride 2: RGBAQ, XYZ2, the colour put
    into its slot by the VIF): two vertices by DIV, two by the EFU, per 24."""
    pos = stride - 1
    ops = lambda k: xform(k, pos, "div" if k % 2 == 0 else "efu") + xyz_out(pos)
    return Kernel(name, ops, 4, 24, stride, MVP, dict(O_PRESET))


def k_tex():
    """S3: ST, RGBAQ, XYZ2; 7 upper ops, DIV every 7; two vertices per
    iteration (P lives 11 cycles: from its MADD until Q is there)."""
    ops = lambda k: xform(k, 2, "div") + st_ops(0, "div") + xyz_out(2)
    return Kernel("vx_tex", ops, 2, 14, 3, MVP, dict(O_PRESET))


def k_tex16():
    """S3b: S3 with ST as V2-16 (ITOF12): 8 upper ops; less for the VIF."""
    ops = lambda k: xform(k, 2, "div") + st16_ops(0) + xyz_out(2)
    return Kernel("vx_tex16", ops, 2, 16, 3, MVP, dict(O_PRESET))


LIGHT1 = [("D0", 4), ("D1", 5), ("D2", 6), ("AMB", 7)]
K_PRESET = {"K": ("w", "lq.w {K}, 7({ctx})")}  # alpha (an integer) in w of AMB


def k_lit():
    """S4: RGBAQ (normal in), XYZ2; one light; 11 upper ops."""
    ops = lambda k: xform(k, 1, "div") + light1(0) + xyz_out(1)
    return Kernel("vx_lit", ops, 2, 22, 2, MVP + LIGHT1, {**O_PRESET, **K_PRESET})


def k_littex():
    """S5: ST, RGBAQ (normal in), XYZ2; one light; 12 upper ops."""
    ops = lambda k: xform(k, 2, "div") + st_ops(0, "div") + light1(1) + xyz_out(2)
    return Kernel("vx_littex", ops, 2, 24, 3, MVP + LIGHT1, {**O_PRESET, **K_PRESET})


SCEI = [("A0", 4), ("A1", 5), ("A2", 6), ("E0", 7), ("E1", 8), ("E2", 9), ("AMB", 10),
        ("MAX", 11), ("MAG", 12), ("GB", 13)]


def k_scei():
    """S6: ps2gl's SCEI shader: three lights, ST, guard band clipping by ADC;
    17 upper ops."""
    ops = lambda k: xform(k, 2, "div") + st_ops(0, "div") + light3(1) + scei_xyz(2)
    return Kernel("vx_scei", ops, 1, 17, 3, MVP + SCEI, {"K": ("w", "lq.w {K}, 10({ctx})")})


KERNELS = {
    "vx_flat": lambda: k_flat("vx_flat", 1),
    "vx_gour": lambda: k_flat("vx_gour", 2),
    "vx_tex": k_tex,
    "vx_lit": k_lit,
    "vx_littex": k_littex,
    "vx_scei": k_scei,
    "vx_tex16": k_tex16,
}

# ---- scheduling --------------------------------------------------------------


class Inst:
    """An op of copy k of the kernel."""

    def __init__(self, op, k):
        self.op, self.k = op, k
        self.id = f"{op.name}.{k}"
        self.c = None

    def g(self, grp):
        return grp if is_special(grp) else f"{grp}.{self.k}"


def is_special(grp):
    return grp in ("Q", "PR", "ACC", "CF") or grp.startswith("VI:")


class Schedule:
    def __init__(self, kern):
        self.kern = kern
        self.ii = kern.ii
        self.insts = []
        for k in range(kern.u):
            for op in kern.ops_for(k):
                self.insts.append(Inst(op, k))
        # the producer of each (group, lane) of a copy
        self.prod = {}
        for i in self.insts:
            if i.op.dst:
                g, lanes = i.op.dst
                for l in (lanes or "-"):
                    self.prod.setdefault((i.k, g), {})[l] = i
        # the inst that an src of inst i reads
        self.srcmap = {}
        for i in self.insts:
            for g, lanes in i.op.srcs:
                if g == "ACC":
                    # the ACC write before i in the same copy
                    prev = [j for j in self.insts if j.k == i.k and j.op.dst and j.op.dst[0] == "ACC"
                            and self.insts.index(j) < self.insts.index(i)]
                    self.srcmap.setdefault(i, []).append((g, lanes, prev[-1]))
                    continue
                pm = self.prod.get((i.k, g))
                if pm is None:
                    continue  # a uniform or a preset
                for l in (lanes or "-"):
                    if l in pm:
                        self.srcmap.setdefault(i, []).append((g, l, pm[l]))
        self.reserved_lower = {self.ii - 2}  # the branch

    # ---- constraints on the placed insts -------------------------------------

    def ok(self, i):
        ii = self.ii
        c = i.c
        slot = c % ii
        for j in self.insts:
            if j is i or j.c is None:
                continue
            if j.op.unit == i.op.unit and j.c % ii == slot:
                return False
            if i.op.res and j.op.res == i.op.res:
                busy = DIV_BUSY if i.op.res == "DIV" else EFU_BUSY
                d = (c - j.c) % ii
                if d < busy or ii - d < busy:
                    return False
        if i.op.unit == "L" and slot in self.reserved_lower:
            return False
        # latency and lifetime of every placed edge into or out of i
        for g, l, r, d in self.edges:
            if (r is not i and d is not i) or r.c is None or d.c is None:
                continue
            if g == "ACC":
                if r.c <= d.c:
                    return False
                continue
            if r.c < d.c + d.op.lat:
                return False
            if g not in ("Q", "PR", "CF") and r.c > d.c + ii:
                return False
        return self.windows_ok()

    def windows_ok(self):
        """Q, P and the clip flags: a read at c of the value of producer d sees
        it if d.c + lat <= c and no other producer's value lands in between;
        ACC: no other ACC op between two links of a chain."""
        ii = self.ii
        for i in self.insts:
            if i.c is None:
                continue
            for g, l, d in self.srcmap.get(i, []):
                if d.c is None:
                    continue
                if g in ("Q", "PR", "CF"):
                    lat = d.op.lat
                    for x in self.insts:
                        if x.c is None or not x.op.dst or x.op.dst[0] != g:
                            continue
                        # instances of x land at x.c + lat + m * ii
                        for m in range(-10, 11):
                            t = x.c + m * ii + lat
                            if x is d and m == 0:
                                continue
                            if d.c + lat < t <= i.c:
                                return False
                    if i.c - (d.c + lat) >= ii and g != "CF":
                        return False
                if g == "ACC":
                    for x in self.insts:
                        if x.c is None or x is d or x is i:
                            continue
                        uses = x.op.dst and x.op.dst[0] == "ACC" or any(s[0] == "ACC" for s in x.op.srcs)
                        if not uses:
                            continue
                        for m in range(-10, 11):
                            t = x.c + m * ii
                            if d.c < t < i.c:
                                return False
                    # the same chain op of other iterations
                    for x in (d, i):
                        for m in (-3, -2, -1, 1, 2, 3):
                            t = x.c + m * ii
                            if d.c < t < i.c:
                                return False
        return True

    def solve(self, limit=3_000_000):
        for plain in (False, True):
            for i in self.insts:
                i.c = None
            if self.solve1(limit, plain):
                return True
        return False

    def solve1(self, limit, plain):
        self.edges = [(g, l, r, d) for r, lst in self.srcmap.items() for g, l, d in lst]
        # EFU vertices first (the tightest), each copy in program order but a
        # load right after its first reader (it is placed back from there)
        order = []
        copies = sorted(range(self.kern.u),
                        key=lambda k: (not any(i.op.res == "EFU" for i in self.insts if i.k == k), k))
        for k in copies:
            ins = [i for i in self.insts if i.k == k]
            loads = [i for i in ins if not self.srcmap.get(i)]
            rest = [i for i in ins if self.srcmap.get(i)]
            for i in rest:
                order.append(i)
                for ld in list(loads):
                    if any(d is ld for _, _, d in self.srcmap.get(i, [])):
                        order.append(ld)
                        loads.remove(ld)
            order += loads
        if plain:
            order = list(self.insts)
        self.order = order
        self.steps = 0
        span = self.ii * 6

        def cands(n, i):
            srcs = [(g, d) for g, l, d in self.srcmap.get(i, []) if d.c is not None]
            if srcs:
                e = max(d.c + (1 if g == "ACC" else d.op.lat) for g, d in srcs)
                return range(e, e + span)
            rds = [(g, r) for g, l, r, d in self.edges if d is i and r.c is not None]
            if rds:
                lat = i.op.lat
                e = min(r.c - (1 if g == "ACC" else lat) for g, r in rds)
                return range(e, e - span, -1)
            if n == 0:
                return range(0, 1)
            return sorted(range(-span, span), key=abs)

        def rec(n):
            if n == len(order):
                return True
            i = order[n]
            for c in cands(n, i):
                self.steps += 1
                if self.steps > limit:
                    return False
                i.c = c
                if self.ok(i) and rec(n + 1):
                    return True
                i.c = None
            return False

        found = rec(0)
        self.total_steps = getattr(self, "total_steps", 0) + self.steps
        if found:
            # stage 0 holds the earliest op; shift by whole iterations only
            # (the slots modulo II stay)
            base = min(i.c for i in self.insts) // self.ii * self.ii
            for i in self.insts:
                i.c -= base
            self.pick_incr()
        return found

    def pick_incr(self):
        used = {i.c % self.ii for i in self.insts if i.op.unit == "L"}
        free = [s for s in range(self.ii) if s not in used and s not in self.reserved_lower
                and s != self.ii - 3 and s < self.ii - 2]
        if not free:
            raise SystemExit(f"{self.kern.name}: no lower slot for the pointer increment")
        self.incr = free[0]

    @property
    def stages(self):
        return max(i.c for i in self.insts) // self.ii + 1


# ---- register allocation and emission ------------------------------------------

def alloc(s):
    """VF registers: uniforms from vf31 down, the per-copy groups from vf01
    up; groups with preset lanes get their own register, the others share
    by modulo lifetime."""
    k = s.kern
    regs = {}
    top = 31
    for g, _ in k.uniforms:
        regs[g] = f"vf{top:02d}"
        top -= 1
    # lifetimes [first def, last read) per group, in modulo time
    life = {}
    for i in s.insts:
        if i.op.dst and not is_special(i.op.dst[0]):
            gk = i.g(i.op.dst[0])
            a, b = life.get(gk, (i.c, i.c))
            life[gk] = (min(a, i.c), max(b, i.c))
    for i in s.insts:
        for g, l, d in s.srcmap.get(i, []):
            if is_special(g):
                continue
            gk = d.g(g)
            a, b = life[gk]
            life[gk] = (a, max(b, i.c))
    phys = []  # [(reg, [(a, b)])]
    nxt = 1
    for gk in sorted(life, key=lambda x: life[x][0]):
        a, b = life[gk]
        g = gk.split(".")[0]
        if b - a > s.ii:
            raise SystemExit(f"{k.name}: {gk} lives {b - a} > II")
        if g in k.presets:
            regs[gk] = f"vf{nxt:02d}"
            nxt += 1
            continue
        placed = False
        for reg, arcs in phys:
            if all(not overlap(a, b, x, y, s.ii) for x, y in arcs):
                arcs.append((a, b))
                regs[gk] = reg
                placed = True
                break
        if not placed:
            phys.append((f"vf{nxt:02d}", [(a, b)]))
            regs[gk] = f"vf{nxt:02d}"
            nxt += 1
    if nxt - 1 > top:
        raise SystemExit(f"{k.name}: needs {nxt - 1} + {31 - top} VF registers")
    return regs


def overlap(a, b, x, y, ii):
    """Half-open arcs [a, b) and [x, y) modulo ii."""
    def cells(p, q):
        return {t % ii for t in range(p, max(q, p + 1))}
    return bool(cells(a, b) & cells(x, y))


def offset(s, i):
    k = s.kern
    slot = i.op.mem[1]
    stage, sl = divmod(i.c, s.ii)
    inc = stage + (1 if sl > s.incr else 0)
    return i.k * k.stride + slot - inc * k.u * k.stride


def fmt(s, regs, i):
    names = {}
    for key, v in regs.items():
        if "." in key:
            g, kk = key.split(".")
            if int(kk) == i.k:
                names[g] = v
        else:
            names[key] = v
    if i.op.mem:
        names["off"] = offset(s, i)
        names["p"] = VI_P
    return i.op.text.format(**names)


def emit(s, regs):
    k = s.kern
    ii = s.ii
    words = [["nop", "nop"] for _ in range(ii)]
    for i in s.insts:
        u = 0 if i.op.unit == "U" else 1
        if words[i.c % ii][u] != "nop" or (u and i.c % ii in (ii - 2, s.incr)):
            raise SystemExit(f"{k.name}: {i.id} collides in word {i.c % ii}")
        words[i.c % ii][u] = fmt(s, regs, i) + \
            f"\t\t; {i.id} stage {i.c // ii}"
    words[ii - 2][1] = f"ibne {VI_P}, {VI_END}, {k.name}_loop"
    words[s.incr][1] = f"iaddiu {VI_P}, {VI_P}, {k.u * k.stride}"
    # header and uniforms
    out = []
    out.append(f"; {k.name}: generated by tools/vusched/vusched.py -- do not edit")
    for line in (k.notes or "").splitlines():
        out.append("; " + line)
    out.append(f"; {k.u} vertices per {ii} cycles ({ii / k.u:g} per vertex), stride {k.stride} qw,"
               f" {s.stages} stages, pad {pad_qw(s)} qw, slop {slop_qw(s)} qw")
    out.append("\t.section .vutext, \"ax\", @progbits")
    out.append(f"\t.globl {k.name}_vu")
    out.append(f"{k.name}_vu:")
    pre = [
        f"xitop {VI_B}",
        f"ilw.y {VI_CTX}, 0({VI_B})",
        f"ilw.z {VI_KICK}, 0({VI_B})",
        f"ilw.w {VI_END}, 0({VI_B})",
        f"lq.xyzw vf01, 1({VI_B})",       # the GIF tag (vf01 is free again once saved)
        f"iaddiu {VI_P}, {VI_B}, 2",
        f"iaddiu {VI_TAG}, {VI_B}, 1",
    ]
    names = dict((g, regs[g]) for g, _ in k.uniforms)
    names["ctx"] = VI_CTX
    for g, qw in k.uniforms:
        pre.append(f"lq.xyzw {regs[g]}, {qw}({VI_CTX})")
    for g, (lanes, text) in k.presets.items():
        for c in range(k.u):
            n = dict(names)
            n[g] = regs[f"{g}.{c}"]
            pre.append(text.format(**n))
    # the GIF tag waits in a register that nothing else uses
    used = set(regs.values())
    free = [f"vf{r:02d}" for r in range(1, 32) if f"vf{r:02d}" not in used]
    if not free:
        raise SystemExit(f"{k.name}: no register for the GIF tag")
    tagreg = free[0]
    pre[4] = f"lq.xyzw {tagreg}, 1({VI_B})"
    for p in pre:
        out.append(f"\tnop\t\t\t\t{p}")
    out.append(f"{k.name}_loop:")
    for up, lo in words:
        lo_t, _, lo_c = lo.partition(";")
        up_t, _, up_c = up.partition(";")
        com = "; ".join(x.strip() for x in (up_c, lo_c) if x.strip())
        out.append(f"\t{up_t.strip():<32}{lo_t.strip():<32}" + (f"; {com}" if com else ""))
    out.append(f"\tnop\t\t\t\tsq.xyzw {tagreg}, 1({VI_B})")
    out.append(f"\tnop\t\t\t\tibeq {VI_KICK}, vi00, {k.name}_end")
    out.append("\tnop\t\t\t\tnop")
    out.append("\tnop\t\t\t\tnop")  # the tag's SQ has landed (4 cycles)
    out.append(f"\tnop\t\t\t\txgkick {VI_TAG}")
    out.append(f"{k.name}_end:")
    out.append("\tnop[e]\t\t\t\tnop")
    out.append("\tnop\t\t\t\tnop")
    out.append(f"\t.globl {k.name}_vu_end")
    out.append(f"{k.name}_vu_end:")
    return "\n".join(out) + "\n", words


def pad_qw(s):
    """Quadwords before vertex 0 that the first iterations store into
    (including the header and the GIF tag)."""
    k = s.kern
    lo = 0
    for i in s.insts:
        if i.op.mem and i.op.mem[0] == "sq":
            st = i.c // s.ii
            # iteration -st .. -1 run this store
            if st:
                lo = min(lo, (-st * k.u + i.k) * k.stride + i.op.mem[1])
    return -lo


def slop_qw(s):
    """Quadwords after the last iteration's vertices that the extra
    iterations store into."""
    k = s.kern
    return (s.stages - 1) * k.u * k.stride


# ---- checking --------------------------------------------------------------------

def check(s, regs, words, iters=12, verbose=False):
    """Run the loop for iters + S - 1 kernel iterations on the cycle model;
    the vertices are iterations 0 .. iters - 1. Returns (cycles per
    iteration, stalls, errors)."""
    k = s.kern
    ii = s.ii
    S = s.stages
    J = iters + S - 1
    errors = []
    by_slot = {}
    for i in s.insts:
        by_slot.setdefault(i.c % ii, []).append(i)
    rev = {}
    for kk, v in regs.items():
        rev.setdefault(v, []).append(kk)
    lane_state = {}       # (reg, lane) -> (tag, ready)
    q = []                # (ready, tag)
    p = []
    cf = []               # (ready, tag)
    acc = None
    vi = {}               # name -> (tag, ready)
    mem = {}              # address -> {lane: tag}
    V0 = 1000             # vertex 0 (symbolic memory)
    ptr = V0
    t = 0
    stalls = 0
    div_free = efu_free = 0
    stores = []

    def latest(lst, now):
        best = None
        for r, tag in lst:
            if r <= now and (best is None or r > best[0]):
                best = (r, tag)
        return best[1] if best else None

    for j in range(J):
        for w in range(ii):
            insts = by_slot.get(w, [])
            # the stall: any VF source lane still in flight, DIV/EFU busy
            need = t
            for i in insts:
                for g, lanes in i.op.srcs:
                    if is_special(g):
                        continue
                    reg = reg_of(regs, g, i.k)
                    for l in lanes:
                        st = lane_state.get((reg, l))
                        if st and st[1] > need:
                            need = st[1]
                if i.op.res == "DIV":
                    need = max(need, div_free)
                if i.op.res == "EFU":
                    need = max(need, efu_free)
            if need > t:
                stalls += need - t
                t = need
            writes = []
            for i in insts:
                it = j - i.c // ii  # the iteration of this instance
                real = 0 <= it < iters
                # reads
                for g, l, d in s.srcmap.get(i, []):
                    want = (d.id, it)
                    if g == "Q":
                        got = latest(q, t)
                    elif g == "PR":
                        got = latest(p, t)
                    elif g == "CF":
                        got = latest(cf, t)
                    elif g == "ACC":
                        got = acc
                    elif g.startswith("VI:"):
                        st = vi.get(g)
                        got = st[0] if st and st[1] <= t else None
                    else:
                        reg = reg_of(regs, g, i.k)
                        st = lane_state.get((reg, l))
                        got = st[0] if st else None
                    if real and got != want:
                        errors.append(f"iter {it} {i.id} @{t}: {g}.{l} got {got}, want {want}")
                # memory
                if i.op.mem:
                    off = offset(s, i)
                    addr = ptr + off
                    want_addr = V0 + (it * k.u + i.k) * k.stride + i.op.mem[1]
                    if addr != want_addr:
                        errors.append(f"iter {it} {i.id}: address {addr}, want {want_addr}")
                    if i.op.mem[0] == "sq":
                        stores.append((it, addr))
                        g, lanes = i.op.srcs[0]
                        reg = reg_of(regs, g, i.k)
                        mem[addr] = {l: lane_state.get((reg, l), (None,))[0] for l in lanes}
                if i.op.dst:
                    g, lanes = i.op.dst
                    tag = (i.id, it)
                    if g == "Q":
                        q.append((t + DIV_LAT, tag))
                        div_free = t + DIV_BUSY
                    elif g == "PR":
                        p.append((t + EFU_LAT, tag))
                        efu_free = t + EFU_BUSY
                    elif g == "CF":
                        cf.append((t + CLIP_LAT, tag))
                    elif g == "ACC":
                        writes.append(("ACC", tag))
                    elif g.startswith("VI:"):
                        vi[g] = (tag, t + i.op.lat)
                    else:
                        reg = reg_of(regs, g, i.k)
                        for l in lanes:
                            writes.append(((reg, l), (tag, t + i.op.lat)))
            for key, val in writes:
                if key == "ACC":
                    acc = val
                else:
                    lane_state[key] = val
            if w == s.incr:
                ptr += k.u * k.stride
            t += 1
    # the stores that land outside the vertices must stay in the pad / slop
    lo = V0 - pad_qw(s)
    hi = V0 + (iters + S - 1) * k.u * k.stride
    for it, a in stores:
        if not lo <= a < hi:
            errors.append(f"store of iteration {it} at {a} outside [{lo}, {hi})")
    # every vertex slot that a store writes got the final value of its iteration
    for i in s.insts:
        if i.op.mem and i.op.mem[0] == "sq":
            g, lanes = i.op.srcs[0]
            for it in range(iters):
                addr = V0 + (it * k.u + i.k) * k.stride + i.op.mem[1]
                got = mem.get(addr, {})
                for l in lanes:
                    pm = s.prod.get((i.k, g), {})
                    want = (pm[l].id, it) if l in pm else None
                    if want and got.get(l) != want:
                        errors.append(f"vertex {it * k.u + i.k} slot {i.op.mem[1]} lane {l}: "
                                      f"{got.get(l)}, want {want}")
    return t / J, stalls, errors


def reg_of(regs, g, k):
    return regs.get(f"{g}.{k}", regs.get(g))


# ---- main ---------------------------------------------------------------------------

def build(name, outdir, verbose):
    kern = KERNELS[name]()
    s = Schedule(kern)
    if not s.solve():
        print(f"{name}: no schedule at II {kern.ii} ({s.steps} steps)")
        return False
    regs = alloc(s)
    text, words = emit(s, regs)
    cyc, stalls, errors = check(s, regs, words)
    # with no stalls, a loop of J iterations takes exactly II per iteration
    ok = stalls == 0 and not errors
    print(f"{name}: II {kern.ii}, {kern.u} vertices, {kern.ii / kern.u:g} cycles/vertex, "
          f"{s.stages} stages, incr in word {s.incr}, {s.steps} steps; checker: "
          f"{cyc:.2f} cycles/iteration, {stalls} stalls, {len(errors)} errors; "
          f"VF {len(set(regs.values()))}")
    for e in errors[:20]:
        print("   ", e)
    if verbose:
        print(text)
    if outdir and ok:
        with open(os.path.join(outdir, name + ".vsm"), "w") as f:
            f.write(text)
        LAYOUT[name] = (kern, s)
    return ok


LAYOUT = {}


def write_header(outdir):
    """vx_kernels.h: the layout of each kernel for demos/vumax/vumax.c."""
    lines = ["/* Generated by tools/vusched/vusched.py -- do not edit. */",
             "#ifndef VX_KERNELS_H", "#define VX_KERNELS_H", ""]
    for name, (k, s) in LAYOUT.items():
        lines.append(f"extern const unsigned char {name}_vu[], {name}_vu_end[];")
    lines.append("")
    lines.append("/* name, code, vertices per iteration (u), II, stride (qw per vertex), stages,")
    lines.append("   pad (qw before vertex 0 that the first iterations store into), slop */")
    lines.append("#define VX_KERNELS(X) \\")
    rows = []
    for name, (k, s) in LAYOUT.items():
        rows.append(f"  X({name}, {k.u}, {k.ii}, {k.stride}, {s.stages}, {pad_qw(s)}, {slop_qw(s)})")
    lines.append(" \\\n".join(rows))
    lines += ["", "#endif", ""]
    with open(os.path.join(outdir, "vx_kernels.h"), "w") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("kernels", nargs="*")
    a = ap.parse_args()
    names = a.kernels or list(KERNELS)
    ok = all([build(n, a.out, a.v) for n in names])
    if a.out and ok and not a.kernels:
        write_header(a.out)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
