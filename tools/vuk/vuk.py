#!/usr/bin/env python3
"""vuk: a tiny kernel language for the PS2 Vector Units, compiled to LLVM IR.

    vuk.py kernel.vuk -o kernel.ll [-H kernel.h] [-D NAME=VALUE ...]

The IR is for `llc -mtriple=vu`. See vuk.md for the language.
"""

import argparse
import math
import re
import struct
import sys

# ---------------------------------------------------------------- lexer

TOKEN_RE = re.compile(r"""
    (?P<ws>\s+|//[^\n]*)
  | (?P<num>\d+\.\d+|\d+)
  | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>\.\.|[-+*/()\[\]{}:;,=.])
""", re.VERBOSE)

KEYWORDS = {"const", "kernel", "shader", "uniform", "out", "state", "unit", "var", "for", "in"}


class VukError(Exception):
    pass


def lex(src):
    toks, pos, line = [], 0, 1
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m:
            raise VukError(f"line {line}: unexpected character {src[pos]!r}")
        kind, text = m.lastgroup, m.group()
        if kind != "ws":
            if kind == "name" and text in KEYWORDS:
                kind = text
            toks.append((kind, text, line))
        line += text.count("\n")
        pos = m.end()
    toks.append(("eof", "", line))
    return toks


# ---------------------------------------------------------------- parser
#
# AST nodes are tuples: ("num", v) ("name", n) ("index", name, expr)
# ("bin", op, a, b) ("neg", a); statements ("var", n, e) ("set", target, e)
# ("for", var, lo, hi, body).

class Parser:
    def __init__(self, toks):
        self.toks, self.i = toks, 0
        self.unit = None

    def peek(self, kind=None, text=None):
        k, t, _ = self.toks[self.i]
        return (kind is None or k == kind) and (text is None or t == text)

    def take(self, kind=None, text=None):
        k, t, line = self.toks[self.i]
        if not self.peek(kind, text):
            raise VukError(f"line {line}: expected {text or kind}, got {t or k!r}")
        self.i += 1
        return t

    def line(self):
        return self.toks[self.i][2]

    def program(self):
        consts, kernels = [], []
        while not self.peek("eof"):
            if self.peek("const"):
                self.take("const")
                name = self.take("name")
                self.take("op", "=")
                consts.append((name, self.expr()))
                self.take("op", ";")
            elif self.peek("shader"):
                kernels.append(self.shader() + (self.unit,))
            elif self.peek("unit"):
                # unit vu0; / unit vu1; for the programs that follow
                self.take("unit")
                unit = self.take("name")
                if unit not in ("vu0", "vu1"):
                    raise VukError(f"line {self.line()}: unknown unit {unit}")
                self.unit = unit
                self.take("op", ";")
            else:
                kernels.append(self.kernel() + (self.unit,))
        return consts, kernels

    def shader(self):
        """shader NAME { uniform ...; in ...; statements; out REG = expr; }"""
        self.take("shader")
        name = self.take("name")
        self.take("op", "{")
        uniforms, inputs, body, states = [], [], [], []
        while not self.peek("op", "}"):
            line = self.line()
            if self.peek("uniform"):
                self.take("uniform")
                uname = self.take("name")
                self.take("op", ":")
                uniforms.append((uname, self.type(), line))
                self.take("op", ";")
            elif self.peek("in"):
                self.take("in")
                iname = self.take("name")
                self.take("op", ":")
                inputs.append((iname, self.take("name"), line))
                self.take("op", ";")
            elif self.peek("state"):
                # state NAME = expr; carried from one vertex to the next
                self.take("state")
                sname = self.take("name")
                self.take("op", "=")
                states.append((sname, self.expr(), line))
                self.take("op", ";")
            elif self.peek("out"):
                self.take("out")
                reg = self.take("name")
                self.take("op", "=")
                e = self.expr()
                self.take("op", ";")
                body.append(("out", reg, e, line))
            else:
                body.append(self.stmt())
        self.take("op", "}")
        return ("shader", name, uniforms, inputs, body, states)

    def kernel(self):
        self.take("kernel")
        name = self.take("name")
        self.take("op", "(")
        params = []
        while not self.peek("op", ")"):
            pname = self.take("name")
            self.take("op", ":")
            params.append((pname, self.type()))
            if not self.peek("op", ")"):
                self.take("op", ",")
        self.take("op", ")")
        return ("kernel", name, params, self.block())

    def type(self):
        base = self.take("name")
        if base not in ("i16", "f32", "f32x4"):
            raise VukError(f"line {self.line()}: unknown type {base}")
        if not self.peek("op", "["):
            return (base, None)
        self.take("op", "[")
        size = None if self.peek("op", "]") else self.expr()
        self.take("op", "]")
        return (base, size if size is not None else "unsized")

    def block(self):
        self.take("op", "{")
        body = []
        while not self.peek("op", "}"):
            body.append(self.stmt())
        self.take("op", "}")
        return body

    def stmt(self):
        line = self.line()
        if self.peek("var"):
            self.take("var")
            name = self.take("name")
            self.take("op", "=")
            e = self.expr()
            self.take("op", ";")
            return ("var", name, e, line)
        if self.peek("for"):
            self.take("for")
            v = self.take("name")
            self.take("in")
            lo = self.expr()
            self.take("op", "..")
            hi = self.expr()
            return ("for", v, lo, hi, self.block(), line)
        target = self.postfix()
        self.take("op", "=")
        e = self.expr()
        self.take("op", ";")
        return ("set", target, e, line)

    def expr(self):
        a = self.term()
        while self.peek("op", "+") or self.peek("op", "-"):
            op = self.take()
            a = ("bin", op, a, self.term())
        return a

    def term(self):
        a = self.unary()
        while self.peek("op", "*") or self.peek("op", "/"):
            op = self.take()
            a = ("bin", op, a, self.unary())
        return a

    def unary(self):
        if self.peek("op", "-"):
            self.take()
            return ("neg", self.unary())
        return self.postfix()

    def postfix(self):
        e = self.primary()
        while self.peek("op", "."):
            self.take()
            lane = self.take("name")
            if lane not in ("x", "y", "z", "w"):
                raise VukError(f"line {self.line()}: unknown lane .{lane}")
            e = ("lane", e, "xyzw".index(lane))
        return e

    def primary(self):
        if self.peek("num"):
            return ("num", self.take())
        if self.peek("op", "("):
            self.take()
            e = self.expr()
            self.take("op", ")")
            return e
        name = self.take("name")
        if self.peek("op", "("):
            self.take()
            args = []
            while not self.peek("op", ")"):
                args.append(self.expr())
                if not self.peek("op", ")"):
                    self.take("op", ",")
            self.take("op", ")")
            return ("call", name, args)
        if self.peek("op", "["):
            self.take()
            idx = self.expr()
            self.take("op", "]")
            return ("index", name, idx)
        return ("name", name)


# ---------------------------------------------------------------- code generation
#
# Values: ("const", python number) for compile-time integers, or
# (type, "%ssa") with type i16, f32x4. Scalar f32 values are kept as splat
# f32x4 vectors, which is what the VU computes with anyway.

class Gen:
    def __init__(self, consts):
        self.consts = consts
        self.lines, self.n, self.label = [], 0, "entry"
        self.uses = set()
        # Shader mode: fast-math flags (FMAC chains, no INF/NaN on the VU),
        # division by lane x, built-in functions.
        self.shader = False
        self.meta = []
        self.attrs = []
        # --entry: the globals of the entry functions (VUEntryLowering)
        self.globals = []

    def tmp(self):
        self.n += 1
        return f"%t{self.n}"

    def emit(self, s):
        self.lines.append("  " + s)

    def block(self, label):
        self.lines.append(f"{label}:")
        self.label = label

    def const_eval(self, e, env=None):
        """Evaluate a compile-time integer expression, or return None."""
        kind = e[0]
        if kind == "num":
            return float(e[1]) if "." in e[1] else int(e[1])
        if kind == "name":
            if env is not None and e[1] in env and env[e[1]][0] == "const":
                return env[e[1]][1]
            return self.consts.get(e[1])
        if kind == "neg":
            v = self.const_eval(e[1], env)
            return None if v is None else -v
        if kind == "bin":
            a, b = self.const_eval(e[2], env), self.const_eval(e[3], env)
            if a is None or b is None:
                return None
            return {"+": a + b, "-": a - b, "*": a * b,
                    "/": (a // b if isinstance(a, int) and isinstance(b, int) else a / b)}[e[1]]
        return None

    # -------- values

    def splat(self, x):
        h = struct_hex(float(x))
        return f"<float {h}, float {h}, float {h}, float {h}>"

    def as_vec(self, v):
        if v[0] == "const":
            return self.splat(v[1])
        if v[0] != "f32x4":
            raise VukError("an i16 value cannot be used as a vector")
        return v[1]

    def as_i16(self, v):
        if v[0] == "const":
            if not isinstance(v[1], int):
                raise VukError(f"{v[1]} is not an integer")
            return str(v[1])
        if v[0] != "i16":
            raise VukError("a vector cannot be used as an index or count")
        return v[1]

    def value(self, e, env):
        c = self.const_eval(e, env)
        if c is not None:
            return ("const", c)
        kind = e[0]
        if kind == "name":
            if e[1] not in env:
                raise VukError(f"unknown name {e[1]}")
            v = env[e[1]]
            if v[0] == "array":
                raise VukError(f"{e[1]} is an array; index it")
            return v
        if kind == "neg":
            v = self.value(e[1], env)
            if v[0] == "i16":
                t = self.tmp()
                self.emit(f"{t} = sub i16 0, {v[1]}")
                return ("i16", t)
            t = self.tmp()
            self.emit(f"{t} = fneg <4 x float> {self.as_vec(v)}")
            return ("f32x4", t)
        if kind == "index":
            return self.load(e[1], e[2], env)
        if kind == "lane":
            v = self.value(e[1], env)
            if v[0] == "const":
                return v
            return ("f32x4", self.lane(self.as_vec(v), e[2]))
        if kind == "call":
            return ("f32x4", self.call(e[1], [self.value(a, env) for a in e[2]]))
        if kind == "bin" and self.shader and "i16" not in (
                self.value_kind(e[2], env), self.value_kind(e[3], env)):
            a, b = self.value(e[2], env), self.value(e[3], env)
            if e[1] == "/":
                return ("f32x4", self.fop("fmul", self.as_vec(a), self.rcp(self.as_vec(b))))
            op = {"+": "fadd", "-": "fsub", "*": "fmul"}[e[1]]
            return ("f32x4", self.fop(op, self.as_vec(a), self.as_vec(b)))
        if kind == "bin":
            a, b = self.value(e[2], env), self.value(e[3], env)
            if "i16" in (a[0], b[0]):
                op = {"+": "add", "-": "sub", "*": "mul"}.get(e[1])
                if op is None:
                    raise VukError("integer division is not supported")
                t = self.tmp()
                self.emit(f"{t} = {op} i16 {self.as_i16(a)}, {self.as_i16(b)}")
                return ("i16", t)
            op = {"+": "fadd", "-": "fsub", "*": "fmul", "/": "fdiv"}[e[1]]
            t = self.tmp()
            self.emit(f"{t} = {op} <4 x float> {self.as_vec(a)}, {self.as_vec(b)}")
            return ("f32x4", t)
        raise VukError(f"cannot compute {e}")

    def value_kind(self, e, env):
        """The type of an expression without generating code for it."""
        if e[0] == "name" and e[1] in env:
            return env[e[1]][0]
        if e[0] == "neg":
            return self.value_kind(e[1], env)
        if e[0] == "bin":
            kinds = (self.value_kind(e[2], env), self.value_kind(e[3], env))
            return "i16" if "i16" in kinds else "f32x4"
        return "f32x4"

    # -------- shader built-ins (all values are f32x4)

    def fop(self, op, a, b, flags="fast"):
        t = self.tmp()
        self.emit(f"{t} = {op} {flags} <4 x float> {a}, {b}")
        return t

    def lane(self, v, k):
        t = self.tmp()
        self.emit(f"{t} = shufflevector <4 x float> {v}, <4 x float> poison, "
                  f"<4 x i32> <i32 {k}, i32 {k}, i32 {k}, i32 {k}>")
        return t

    def scalar(self, v):
        t = self.tmp()
        self.emit(f"{t} = extractelement <4 x float> {v}, i32 0")
        return t

    def splat_of(self, f):
        t, u = self.tmp(), self.tmp()
        self.emit(f"{t} = insertelement <4 x float> poison, float {f}, i32 0")
        self.emit(f"{u} = shufflevector <4 x float> {t}, <4 x float> poison, "
                  f"<4 x i32> zeroinitializer")
        return u

    def rcp(self, v):
        """1 / v.x in all lanes: one DIV."""
        q = self.tmp()
        self.emit(f"{q} = fdiv fast float 1.0, {self.scalar(v)}")
        return self.splat_of(q)

    def sqrt_x(self, v):
        self.uses.add("sqrt")
        q = self.tmp()
        self.emit(f"{q} = call fast float @llvm.sqrt.f32(float {self.scalar(v)})")
        return q

    def intrinsic(self, name, *args):
        self.uses.add(name)
        t = self.tmp()
        a = ", ".join(f"<4 x float> {x}" for x in args)
        self.emit(f"{t} = call fast <4 x float> @llvm.{name}.v4f32({a})")
        return t

    def floor(self, v):
        """floor(v) for |v| < 2^22. Adding and subtracting 1.5 * 2^23 drops the
        fraction of the positive sum (the VU rounds towards zero), but the
        PS2's VU cuts the smaller operand of an add to one bit below the
        sum's last first (PCSX2 does not; tests/ps2/floor.c): for a negative v
        with a fraction below 0.5, t = (v + c) - c is floor(v) + 1 (it is
        never below floor(v)). The difference t - v, scaled into 0..1, takes
        that one off."""
        # not reassociated: that would fold (v + c) - c to v
        c = self.splat(12582912.0)
        exact = "nnan ninf nsz"
        t = self.fop("fsub", self.fop("fadd", v, c, exact), c, exact)
        over = self.fop("fmul", self.fop("fsub", t, v, exact), self.splat(1e30), exact)
        one = self.intrinsic("minnum", self.intrinsic("maxnum", over, self.splat(0.0)),
                             self.splat(1.0))
        return self.fop("fsub", t, one, exact)

    def sin(self, v):
        """sin(v): u = v / 2pi - round(v / 2pi) in [-0.5, 0.5), then an odd
        polynomial in u (FMAC only; ESIN takes 29 cycles and is not pipelined)."""
        t = self.fop("fmul", v, self.splat(0.15915494309189535))
        u = self.fop("fsub", t, self.floor(self.fop("fadd", t, self.splat(0.5))))
        u2 = self.fop("fmul", u, u)
        # sin(2 pi u) for |u| <= 0.5, degree 9, error 6e-6 (weighted least
        # squares fit)
        coeffs = [6.283055791, -41.33121387, 81.36680843, -74.4779649, 32.78132493]
        y = self.splat(coeffs[-1])
        for c in reversed(coeffs[:-1]):
            y = self.fop("fadd", self.fop("fmul", y, u2), self.splat(c))
        return self.fop("fmul", y, u)

    def call(self, name, args):
        vec = [self.as_vec(a) for a in args]
        def need(n):
            if len(vec) != n:
                raise VukError(f"{name}() takes {n} arguments")
        if name in ("min", "max"):
            need(2)
            return self.intrinsic("minnum" if name == "min" else "maxnum", *vec)
        if name == "abs":
            need(1)
            return self.intrinsic("fabs", vec[0])
        if name == "clamp":
            need(3)
            return self.intrinsic("minnum", self.intrinsic("maxnum", vec[0], vec[1]), vec[2])
        if name == "lerp":
            need(3)
            return self.fop("fadd", vec[0], self.fop("fmul", self.fop("fsub", vec[1], vec[0]), vec[2]))
        if name == "dot3":
            need(2)
            m = self.fop("fmul", vec[0], vec[1])
            return self.fop("fadd", self.fop("fadd", self.lane(m, 0), self.lane(m, 1)),
                            self.lane(m, 2))
        if name == "rcp":
            need(1)
            return self.rcp(vec[0])
        if name == "sqrt":
            need(1)
            return self.splat_of(self.sqrt_x(vec[0]))
        if name == "rsqrt":
            need(1)
            q = self.tmp()
            self.emit(f"{q} = fdiv fast float 1.0, {self.sqrt_x(vec[0])}")
            return self.splat_of(q)
        if name == "normalize":
            need(1)
            d = self.call("dot3", [("f32x4", vec[0]), ("f32x4", vec[0])])
            return self.fop("fmul", vec[0], self.call("rsqrt", [("f32x4", d)]))
        if name == "floor":
            need(1)
            return self.floor(vec[0])
        if name in ("sin", "cos"):
            need(1)
            x = vec[0] if name == "sin" else self.fop("fadd", vec[0], self.splat(1.5707963267948966))
            return self.sin(x)
        m = re.fullmatch(r"(ftoi|itof)(0|4|12|15)", name)
        if m:
            need(1)
            k = int(m.group(2))
            if m.group(1) == "ftoi":
                v = vec[0] if k == 0 else self.fop("fmul", vec[0], self.splat(float(1 << k)))
                i, f = self.tmp(), self.tmp()
                self.emit(f"{i} = fptosi <4 x float> {v} to <4 x i32>")
                self.emit(f"{f} = bitcast <4 x i32> {i} to <4 x float>")
                return f
            i, f = self.tmp(), self.tmp()
            self.emit(f"{i} = bitcast <4 x float> {vec[0]} to <4 x i32>")
            self.emit(f"{f} = sitofp <4 x i32> {i} to <4 x float>")
            return f if k == 0 else self.fop("fmul", f, self.splat(1.0 / (1 << k)))
        if name == "vec":
            need(4)
            t = "poison"
            for k, v in enumerate(vec):
                n = self.tmp()
                self.emit(f"{n} = insertelement <4 x float> {t}, float {self.scalar(v)}, i32 {k}")
                t = n
            return t
        if name in ("with_x", "with_y", "with_z", "with_w"):
            need(2)
            n = self.tmp()
            self.emit(f"{n} = insertelement <4 x float> {vec[0]}, float {self.scalar(vec[1])}, "
                      f"i32 {'xyzw'.index(name[-1])}")
            return n
        raise VukError(f"unknown function {name}()")

    def address(self, name, idx, env):
        arr = env.get(name)
        if arr is None or arr[0] != "array":
            raise VukError(f"{name} is not an array")
        _, elem, base = arr
        if elem != "f32x4":
            raise VukError(f"{name} is a const array; only f32x4 arrays are in memory")
        i = self.value(idx, env)
        if i[0] == "const":
            return str(base + i[1])
        t = self.tmp()
        self.emit(f"{t} = add i16 {self.as_i16(i)}, {base}")
        return t

    def item_ptr(self, name, idx, env):
        """--entry: the pointer to item idx of the items array."""
        i = self.as_i16(self.value(idx, env))
        t = self.tmp()
        self.emit(f"{t} = getelementptr [0 x <4 x float>], ptr addrspace(4) @{name}, "
                  f"i16 0, i16 {i}")
        return t

    def load(self, name, idx, env):
        arr = env.get(name)
        if arr is not None and arr[0] == "items":
            p, t = self.item_ptr(name, idx, env), self.tmp()
            self.emit(f"{t} = load <4 x float>, ptr addrspace(4) {p}")
            return ("f32x4", t)
        if arr is not None and arr[0] == "table":
            # const f32[] parameter: loaded at kernel entry, index must be constant
            k = self.const_eval(idx, env)
            if not isinstance(k, int) or not 0 <= k < len(arr[1]):
                raise VukError(f"{name}[...] needs a constant index in range")
            return ("f32x4", arr[1][k])
        a = self.address(name, idx, env)
        t = self.tmp()
        self.emit(f"{t} = call <4 x float> @llvm.vu.lq(i16 {a})")
        self.uses.add("lq")
        return ("f32x4", t)

    # -------- statements

    def stmts(self, body, env):
        for s in body:
            self.stmt(s, env)

    def stmt(self, s, env):
        try:
            kind = s[0]
            if kind == "var":
                env[s[1]] = self.value(s[2], env)
            elif kind == "set":
                target, e = s[1], s[2]
                v = self.value(e, env)
                if target[0] == "name":
                    if target[1] not in env or env[target[1]][0] in ("array", "table", "items"):
                        raise VukError(f"cannot assign to {target[1]}")
                    env[target[1]] = v
                elif target[0] == "index" and env.get(target[1], ("",))[0] == "items":
                    p = self.item_ptr(target[1], target[2], env)
                    self.emit(f"store <4 x float> {self.as_vec(v)}, ptr addrspace(4) {p}")
                elif target[0] == "index":
                    a = self.address(target[1], target[2], env)
                    self.emit(f"call void @llvm.vu.sq(<4 x float> {self.as_vec(v)}, i16 {a})")
                    self.uses.add("sq")
                else:
                    raise VukError("cannot assign to an expression")
            elif kind == "for":
                self.loop(s, env)
        except VukError as err:
            if not str(err).startswith("line"):
                raise VukError(f"line {s[-1]}: {err}") from None
            raise

    def loop(self, s, env):
        _, var, lo_e, hi_e, body, _ = s
        lo, hi = self.const_eval(lo_e, env), self.const_eval(hi_e, env)
        # A var of the body lives in one iteration of it.
        outer = set(env)

        def end_scope():
            for k in [k for k in env if k not in outer]:
                del env[k]

        if lo is not None and hi is not None:
            # compile-time bounds: unroll
            for k in range(lo, hi):
                env[var] = ("const", k)
                self.stmts(body, env)
                end_scope()
            env.pop(var, None)
            return

        # run-time loop: i in lo..hi, with phis for the variables it changes.
        # A variable with a constant value that the loop assigns gets a value
        # at run time: a float is a vector, an integer an i16.
        for name in assigned_names(body):
            v = env.get(name)
            if v is not None and v[0] == "const":
                env[name] = (("f32x4", self.splat(v[1])) if isinstance(v[1], float)
                             else ("i16", str(v[1])))
        lo_v = self.as_i16(self.value(lo_e, env))
        hi_v = self.as_i16(self.value(hi_e, env))
        n = self.n = self.n + 1
        head, done = f"loop{n}", f"done{n}"
        carried = [k for k, v in env.items() if v[0] in ("i16", "f32x4")]

        empty = self.tmp()
        self.emit(f"{empty} = icmp sge i16 {lo_v}, {hi_v}")
        pre = self.label
        self.emit(f"br i1 {empty}, label %{done}, label %{head}")
        self.block(head)
        i = f"%i{n}"
        phi_at = len(self.lines)
        before = {k: env[k] for k in carried}
        phis = {k: f"%p{n}_{k}" for k in carried}
        for k in carried:
            env[k] = (before[k][0], phis[k])
        env[var] = ("i16", i)
        self.stmts(body, env)
        nxt = self.tmp()
        self.emit(f"{nxt} = add i16 {i}, 1")
        cond = self.tmp()
        self.emit(f"{cond} = icmp eq i16 {nxt}, {hi_v}")
        latch = self.label
        self.emit(f"br i1 {cond}, label %{done}, label %{head}")

        header = [f"  {i} = phi i16 [ {lo_v}, %{pre} ], [ {nxt}, %{latch} ]"]
        after = {}
        for k in carried:
            if env[k][1] == phis[k]:
                # not changed in the loop: use the value from before it
                pat = re.compile(re.escape(phis[k]) + r"\b")
                self.lines[phi_at:] = [pat.sub(before[k][1], l) for l in self.lines[phi_at:]]
                env[k] = before[k]
                continue
            ty = "i16" if before[k][0] == "i16" else "<4 x float>"
            header.append(f"  {phis[k]} = phi {ty} [ {before[k][1]}, %{pre} ], "
                          f"[ {env[k][1]}, %{latch} ]")
            after[k] = (before[k][0], env[k][1], phis[k], ty)
        self.lines[phi_at:phi_at] = header

        self.block(done)
        end_scope()
        for k, (ty0, last, phi, ty) in after.items():
            t = f"%x{n}_{k}"
            self.emit(f"{t} = phi {ty} [ {before[k][1]}, %{pre} ], [ {last}, %{latch} ]")
            env[k] = (ty0, t)


def assigned_names(body):
    """The variables that the statements assign, also in nested loops."""
    names = set()
    for st in body:
        if st[0] == "set" and st[1][0] == "name":
            names.add(st[1][1])
        elif st[0] == "for":
            names |= assigned_names(st[4])
    return names


def struct_hex(x):
    """An f32 constant in LLVM's hex form (the double with the f32's value)."""
    f = struct.unpack("<f", struct.pack("<f", x))[0]
    return "0x%016X" % struct.unpack("<Q", struct.pack("<d", f))[0]


def define(gen, name, unit):
    """The function header; a unit is the target-cpu of the function."""
    if unit is None:
        return f"define void @{name}() {{"
    gen.attrs.append(f'attributes #{len(gen.attrs)} = {{ "target-cpu"="{unit}" }}')
    return f"define void @{name}() #{len(gen.attrs) - 1} {{"


def compile_kernel(gen, kernel, header):
    name, params, body, unit = kernel
    env, addr = {}, 0
    gen.lines, gen.label = [], "entry"
    gen.lines.append(define(gen, name, unit))
    gen.block("entry")
    header.append(f"/* kernel {name}: VU data memory layout, in quadwords */")
    for i, (pname, (ty, size)) in enumerate(params):
        tag = f"VUK_{name.upper()}_{pname.upper()}"
        if size is None and ty == "i16":
            t = gen.tmp()
            gen.emit(f"{t} = call i16 @llvm.vu.ilw(i16 {addr}, i32 0)")
            gen.uses.add("ilw")
            env[pname] = ("i16", t)
            header.append(f"#define {tag} {addr}  /* i16, field x */")
            addr += 1
        elif size is None and ty == "f32":
            t = gen.tmp()
            gen.emit(f"{t} = call <4 x float> @llvm.vu.lq(i16 {addr})")
            s = gen.tmp()
            gen.emit(f"{s} = shufflevector <4 x float> {t}, <4 x float> poison, <4 x i32> zeroinitializer")
            gen.uses.add("lq")
            env[pname] = ("f32x4", s)
            header.append(f"#define {tag} {addr}  /* f32, field x */")
            addr += 1
        elif ty == "f32" and size != "unsized":
            count = gen.const_eval(size)
            if not isinstance(count, int) or count <= 0:
                raise VukError(f"{pname}: size must be a positive constant")
            vals = []
            for q in range((count + 3) // 4):
                t = gen.tmp()
                gen.emit(f"{t} = call <4 x float> @llvm.vu.lq(i16 {addr + q})")
                gen.uses.add("lq")
                for f in range(4):
                    if len(vals) == count:
                        break
                    s = gen.tmp()
                    gen.emit(f"{s} = shufflevector <4 x float> {t}, <4 x float> poison, "
                             f"<4 x i32> <i32 {f}, i32 {f}, i32 {f}, i32 {f}>")
                    vals.append(s)
            env[pname] = ("table", vals)
            header.append(f"#define {tag} {addr}  /* f32[{count}], 4 per quadword */")
            addr += (count + 3) // 4
        elif ty == "f32x4" and size is not None:
            if size == "unsized":
                if i != len(params) - 1:
                    raise VukError(f"{pname}: an unsized array must be the last parameter")
                count = None
            else:
                count = gen.const_eval(size)
            env[pname] = ("array", "f32x4", addr)
            header.append(f"#define {tag} {addr}  /* f32x4[{count if count else ''}] */")
            addr += count or 0
        else:
            raise VukError(f"{pname}: unsupported parameter type")
    header.append(f"#define VUK_{name.upper()}_SIZE {addr}  /* quadwords before any unsized array */")
    # how a host runtime launches it (maxsdk MX_VUK_KERNEL): the code starts
    # at micro address 0, each item is one f32x4 quadword sent as UNPACK
    # V4-32 and transformed in place, at fixed addresses (no double buffer)
    header.append(f"#define VUK_{name.upper()}_ENTRY 0  /* micro address, 64-bit units */")
    header.append(f"#define VUK_{name.upper()}_ITEM_QW 1  /* quadwords per item */")
    header.append(f"#define VUK_{name.upper()}_FORMAT 0x0C  /* UNPACK V4-32 */")
    header.append(f"#define VUK_{name.upper()}_DBUF 0  /* no double buffering */")
    header.append("")
    gen.stmts(body, env)
    gen.emit("ret void")
    gen.lines.append("}")
    return gen.lines


def entry_attrs(gen, name, unit, kind, extra=""):
    """--entry: the attributes of an entry function for VUEntryLowering."""
    a = f'"vu-entry"="{kind}" "vu-header-prefix"="VUK_"'
    if unit:
        a += f' "target-cpu"="{unit}"'
    gen.attrs.append(f"attributes #{len(gen.attrs)} = {{ {a}{extra} }}")
    return f"define void @{name}() #{len(gen.attrs) - 1} {{"


def entry_meta(gen, text):
    m = f"!{len(gen.meta)}"
    gen.meta.append(f"{m} = !{{{text}}}")
    return m


def entry_uniform(gen, name, qw, count):
    """A uniform of count quadwords at quadword qw (from 1) of the layout."""
    m = entry_meta(gen, f"i32 {(qw - 1) * 16}")
    gen.globals.append(f"@{name} = external addrspace(1) global [{count} x <4 x float>], "
                       f"!vu.uniform {m}")

    def load(k):
        p, t = gen.tmp(), gen.tmp()
        gen.emit(f"{p} = getelementptr [{count} x <4 x float>], ptr addrspace(1) @{name}, "
                 f"i16 0, i16 {k}")
        gen.emit(f"{t} = load <4 x float>, ptr addrspace(1) {p}")
        return t
    return load


def compile_kernel_entry(gen, kernel):
    """--entry: a kernel as a "compute" entry function that runs once (its
    loops are its own). The first parameter, an i16, is the item count; the
    others are uniforms at the same quadwords as without --entry; the unsized
    array is the items."""
    name, params, body, unit = kernel
    env, addr = {}, 0
    gen.lines, gen.label = [], "entry"
    gen.lines.append(entry_attrs(gen, name, unit, "compute"))
    gen.block("entry")
    for i, (pname, (ty, size)) in enumerate(params):
        if i == 0:
            if ty != "i16" or size is not None:
                raise VukError(f"{pname}: with --entry, the first parameter is the i16 count")
            t = gen.tmp()
            gen.emit(f"{t} = call i16 @llvm.vu.item.count()")
            gen.uses.add("count")
            env[pname] = ("i16", t)
            addr = 1
        elif size is None and ty == "i16":
            m = entry_meta(gen, f"i32 {(addr - 1) * 16}")
            gen.globals.append(f"@{pname} = external addrspace(1) global i16, !vu.uniform {m}")
            t = gen.tmp()
            gen.emit(f"{t} = load i16, ptr addrspace(1) @{pname}")
            env[pname] = ("i16", t)
            addr += 1
        elif size is None and ty == "f32":
            v = entry_uniform(gen, pname, addr, 1)(0)
            sp = gen.tmp()
            gen.emit(f"{sp} = shufflevector <4 x float> {v}, <4 x float> poison, <4 x i32> zeroinitializer")
            env[pname] = ("f32x4", sp)
            addr += 1
        elif ty == "f32" and size != "unsized":
            count = gen.const_eval(size)
            if not isinstance(count, int) or count <= 0:
                raise VukError(f"{pname}: size must be a positive constant")
            nq = (count + 3) // 4
            load = entry_uniform(gen, pname, addr, nq)
            vals = []
            for q in range(nq):
                t = load(q)
                for f in range(4):
                    if len(vals) == count:
                        break
                    sp = gen.tmp()
                    gen.emit(f"{sp} = shufflevector <4 x float> {t}, <4 x float> poison, "
                             f"<4 x i32> <i32 {f}, i32 {f}, i32 {f}, i32 {f}>")
                    vals.append(sp)
            env[pname] = ("table", vals)
            addr += nq
        elif ty == "f32x4" and size == "unsized":
            if i != len(params) - 1:
                raise VukError(f"{pname}: an unsized array must be the last parameter")
            gen.globals.append(f"@{pname} = external addrspace(4) global [0 x <4 x float>]")
            env[pname] = ("items", pname)
        else:
            raise VukError(f"{pname}: not supported with --entry")
    gen.stmts(body, env)
    gen.emit("ret void")
    gen.lines.append("}")
    return gen.lines


def compile_shader_entry(gen, shader, buf_qw):
    """--entry: a shader as a "vertex" entry function for one vertex."""
    _, name, uniforms, inputs, body, states, unit = shader
    if unit == "vu0":
        raise VukError(f"shader {name}: shaders run on VU1 (XGKICK)")
    env = {}
    gen.lines, gen.label = [], "entry"
    gen.shader = True
    gen.lines.append(entry_attrs(gen, name, None, "vertex", f' "vu-buffer-qw"="{buf_qw}"'))
    gen.block("entry")
    addr = 1
    for uname, (ty, size), line in uniforms:
        if ty == "f32" and size is None:
            env[uname] = ("f32x4", gen.lane(entry_uniform(gen, uname, addr, 1)(0), 0))
            addr += 1
        elif ty == "f32x4" and size is None:
            env[uname] = ("f32x4", entry_uniform(gen, uname, addr, 1)(0))
            addr += 1
        elif ty == "f32x4" and size not in (None, "unsized"):
            count = gen.const_eval(size)
            if not isinstance(count, int) or count <= 0:
                raise VukError(f"line {line}: {uname}: size must be a positive constant")
            load = entry_uniform(gen, uname, addr, count)
            env[uname] = ("table", [load(k) for k in range(count)])
            addr += count
        else:
            raise VukError(f"line {line}: {uname}: a uniform is f32, f32x4 or f32x4[K]")
    for k, (iname, fmt, line) in enumerate(inputs):
        m = entry_meta(gen, f'i32 {k}, !"{fmt}"')
        gen.globals.append(f"@{iname} = external addrspace(2) global <4 x float>, !vu.input {m}")
        t = gen.tmp()
        gen.emit(f"{t} = load <4 x float>, ptr addrspace(2) @{iname}")
        env[iname] = ("f32x4", t)
    # The states keep their value from vertex to vertex (!vu.carry).
    for sname, e, line in states:
        v = gen.const_eval(e, env)
        if v is None:
            raise VukError(f"line {line}: with --entry, a state starts with a constant")
        m = entry_meta(gen, "")
        gen.globals.append(f"@{sname} = internal addrspace(5) global <4 x float> "
                           f"splat (float {struct_hex(float(v))}), !vu.carry {m}")
        t = gen.tmp()
        gen.emit(f"{t} = load <4 x float>, ptr addrspace(5) @{sname}")
        env[sname] = ("f32x4", t)
    outs, values = 0, []
    for st in body:
        if st[0] == "out":
            _, reg, e, line = st
            if reg not in GIF_REGS:
                raise VukError(f"line {line}: unknown GIF register {reg}")
            try:
                v = gen.as_vec(gen.value(e, env))
            except VukError as err:
                raise VukError(f"line {line}: {err}") from None
            m = entry_meta(gen, f'i32 {outs}, !"{reg}"')
            gen.globals.append(f"@{name}.out{outs} = external addrspace(3) global <4 x float>, "
                               f"!vu.output {m}")
            values.append((outs, v))
            outs += 1
        else:
            gen.stmt(st, env)
    # The outputs are stored after the body, as without --entry.
    for j, v in values:
        gen.emit(f"store <4 x float> {v}, ptr addrspace(3) @{name}.out{j}")
    for sname, e, line in states:
        gen.emit(f"store <4 x float> {gen.as_vec(env[sname])}, ptr addrspace(5) @{sname}")
    gen.emit("ret void")
    gen.lines.append("}")
    gen.shader = False
    return gen.lines


DECLS = {
    "lq": "declare <4 x float> @llvm.vu.lq(i16)",
    "sq": "declare void @llvm.vu.sq(<4 x float>, i16)",
    "ilw": "declare i16 @llvm.vu.ilw(i16, i32)",
    "xtop": "declare i16 @llvm.vu.xtop()",
    "xgkick": "declare void @llvm.vu.xgkick(i16)",
    "minnum": "declare <4 x float> @llvm.minnum.v4f32(<4 x float>, <4 x float>)",
    "maxnum": "declare <4 x float> @llvm.maxnum.v4f32(<4 x float>, <4 x float>)",
    "fabs": "declare <4 x float> @llvm.fabs.v4f32(<4 x float>)",
    "sqrt": "declare float @llvm.sqrt.f32(float)",
    "count": "declare i16 @llvm.vu.item.count()",
}

# ---------------------------------------------------------------- shaders
#
# UNPACK formats of shader inputs: (vn << 2 | vl, bytes per vertex). A "u"
# suffix zero-extends the 16- and 8-bit formats instead of sign-extending.
FORMATS = {
    "s32": (0x0, 4), "s16": (0x1, 2), "s8": (0x2, 1),
    "v2_32": (0x4, 8), "v2_16": (0x5, 4), "v2_8": (0x6, 2),
    "v3_32": (0x8, 12), "v3_16": (0x9, 6), "v3_8": (0xA, 3),
    "v4_32": (0xC, 16), "v4_16": (0xD, 8), "v4_8": (0xE, 4), "v4_5": (0xF, 2),
}

# GIF registers that a shader can write per vertex (PACKED mode).
GIF_REGS = {"rgbaq": 0x1, "st": 0x2, "uv": 0x3, "xyzf2": 0x4, "xyz2": 0x5,
            "fog": 0xA, "xyzf3": 0xC, "xyz3": 0xD}


def compile_shader(gen, shader, header, buf_qw):
    """A VU1 render program for one double buffer of buf_qw quadwords at
    xtop: [GIF tag][uniforms][input items][output: GIF tag, registers]. The
    EE writes the GIF tag with NLOOP = the vertex count."""
    _, name, uniforms, inputs, body, states, unit = shader
    if unit == "vu0":
        raise VukError(f"shader {name}: shaders run on VU1 (XGKICK)")
    up = name.upper()
    env = {}
    gen.lines, gen.label = [], "entry"
    gen.shader = True
    gen.uses |= {"lq", "sq", "ilw", "xtop", "xgkick"}
    gen.lines.append(define(gen, name, "vu1"))
    gen.block("entry")
    gen.emit("%top = call i16 @llvm.vu.xtop()")
    gen.emit("%nloop = call i16 @llvm.vu.ilw(i16 %top, i32 0)")
    gen.emit("%n = and i16 %nloop, 32767")
    gen.emit("%tag = call <4 x float> @llvm.vu.lq(i16 %top)")

    header.append(f"/* shader {name}: one VU1 buffer of {buf_qw} quadwords at xtop */")
    addr = 1
    for uname, (ty, size), line in uniforms:
        tag = f"VUK_{up}_{uname.upper()}"
        def load(q):
            a, t = gen.tmp(), gen.tmp()
            gen.emit(f"{a} = add i16 %top, {q}")
            gen.emit(f"{t} = call <4 x float> @llvm.vu.lq(i16 {a})")
            return t
        if ty == "f32" and size is None:
            env[uname] = ("f32x4", gen.lane(load(addr), 0))
            header.append(f"#define {tag} {addr}  /* f32, field x */")
            addr += 1
        elif ty == "f32x4" and size is None:
            env[uname] = ("f32x4", load(addr))
            header.append(f"#define {tag} {addr}  /* f32x4 */")
            addr += 1
        elif ty == "f32x4" and size not in (None, "unsized"):
            count = gen.const_eval(size)
            if not isinstance(count, int) or count <= 0:
                raise VukError(f"line {line}: {uname}: size must be a positive constant")
            env[uname] = ("table", [load(addr + k) for k in range(count)])
            header.append(f"#define {tag} {addr}  /* f32x4[{count}] */")
            addr += count
        else:
            raise VukError(f"line {line}: {uname}: a uniform is f32, f32x4 or f32x4[K]")
    uni_qw = addr - 1

    # The items: one quadword per input. Batches start on a quadword of every
    # input stream, and an UNPACK ends on one (the VIF reads the rest of a
    # quadword as VIF codes), so up to align - 1 more vertices are unpacked.
    item_qw = len(inputs)
    if not item_qw:
        raise VukError(f"shader {name}: no inputs")
    align = 1
    for iname, fmt, line in inputs:
        base = fmt[:-1] if fmt.endswith("u") and fmt[:-1] in FORMATS else fmt
        if base not in FORMATS:
            raise VukError(f"line {line}: {iname}: unknown format {fmt}")
        nbytes = FORMATS[base][1]
        align = max(align, 16 // math.gcd(16, nbytes))
    outs = [s for s in body if s[0] == "out"]
    if not outs:
        raise VukError(f"shader {name}: no outputs")
    if len(outs) > 15:
        raise VukError(f"shader {name}: more than 15 outputs")
    out_qw = len(outs)
    # the GIF tag and uniforms, the items, the GIF tag and registers out
    max_n = (buf_qw - 2 - uni_qw - (align - 1) * item_qw) // (item_qw + out_qw)
    if max_n < align:
        raise VukError(f"shader {name}: the buffer has no room for a batch")
    in_at = 1 + uni_qw
    out_at = in_at + (max_n + align - 1) * item_qw
    if out_at + 1 + max_n * out_qw > buf_qw or out_at > 1023:
        raise VukError(f"shader {name}: the output does not fit the buffer")

    # The states start each batch with their value, in all fields.
    state_init = {}
    for sname, e, line in states:
        try:
            state_init[sname] = gen.as_vec(gen.value(e, env))
        except VukError as err:
            raise VukError(f"line {line}: {err}") from None
    gen.emit(f"%in = add i16 %top, {in_at}")
    gen.emit(f"%out = add i16 %top, {out_at}")
    gen.emit("call void @llvm.vu.sq(<4 x float> %tag, i16 %out)")
    gen.emit("%o1 = add i16 %out, 1")
    gen.emit("%empty = icmp eq i16 %n, 0")
    gen.emit("br i1 %empty, label %done, label %loop")

    # Inputs and outputs are apart: alias scopes let loads of the next vertex
    # pass the stores of this one.
    m = len(gen.meta)
    dom, sin, sout, lin, lout = (f"!{m + k}" for k in range(5))
    gen.meta += [f"{dom} = distinct !{{{dom}, i1 true, !\"{name}\"}}",
                 f"{sin} = distinct !{{{sin}, {dom}, !\"in\"}}",
                 f"{sout} = distinct !{{{sout}, {dom}, !\"out\"}}",
                 f"{lin} = !{{{sin}}}", f"{lout} = !{{{sout}}}"]

    gen.block("loop")
    phi_at = len(gen.lines)
    for sname in state_init:
        env[sname] = ("f32x4", f"%s_{sname}")
    for k, (iname, fmt, line) in enumerate(inputs):
        a, t = gen.tmp(), gen.tmp()
        gen.emit(f"{a} = add i16 %pi, {k}")
        gen.emit(f"{t} = call <4 x float> @llvm.vu.lq(i16 {a}), !alias.scope {lin}, !noalias {lout}")
        env[iname] = ("f32x4", t)
    values = []
    for st in body:
        if st[0] == "out":
            _, reg, e, line = st
            if reg not in GIF_REGS:
                raise VukError(f"line {line}: unknown GIF register {reg}")
            try:
                values.append((reg, gen.as_vec(gen.value(e, env))))
            except VukError as err:
                raise VukError(f"line {line}: {err}") from None
        else:
            gen.stmt(st, env)
    for j, (reg, v) in enumerate(values):
        a = gen.tmp()
        gen.emit(f"{a} = add i16 %po, {j}")
        gen.emit(f"call void @llvm.vu.sq(<4 x float> {v}, i16 {a}), !alias.scope {lout}, !noalias {lin}")
    latch = gen.label
    gen.emit(f"%pi.next = add i16 %pi, {item_qw}")
    gen.emit(f"%po.next = add i16 %po, {out_qw}")
    gen.emit("%i.next = add i16 %i, 1")
    gen.emit("%last = icmp eq i16 %i.next, %n")
    gen.emit("br i1 %last, label %done, label %loop")
    gen.lines[phi_at:phi_at] = [
        f"  %i = phi i16 [ 0, %entry ], [ %i.next, %{latch} ]",
        f"  %pi = phi i16 [ %in, %entry ], [ %pi.next, %{latch} ]",
        f"  %po = phi i16 [ %o1, %entry ], [ %po.next, %{latch} ]"] + [
        f"  %s_{sname} = phi <4 x float> [ {init}, %entry ], "
        f"[ {gen.as_vec(env[sname])}, %{latch} ]" for sname, init in state_init.items()]
    gen.block("done")
    gen.emit("call void @llvm.vu.xgkick(i16 %out)")
    gen.emit("ret void")
    gen.lines.append("}")
    gen.shader = False

    regs = 0
    for j, (reg, _) in enumerate(values):
        regs |= GIF_REGS[reg] << (4 * j)
    header.append(f"#define VUK_{up}_ENTRY 0  /* micro address, 64-bit units */")
    header.append(f"#define VUK_{up}_BUF_QW {buf_qw}  /* double buffer size */")
    header.append(f"#define VUK_{up}_UNI_QW {uni_qw}  /* uniforms, from quadword 1 */")
    header.append(f"#define VUK_{up}_ITEM_QW {item_qw}  /* quadwords per vertex in */")
    header.append(f"#define VUK_{up}_OUT_QW {out_qw}  /* GIF registers per vertex */")
    header.append(f"#define VUK_{up}_MAX_N {max_n}  /* vertices per batch */")
    header.append(f"#define VUK_{up}_ALIGN {align}  /* batches start at multiples of it */")
    header.append(f"#define VUK_{up}_IN_AT {in_at}  /* first item */")
    header.append(f"#define VUK_{up}_OUT_AT {out_at}  /* the output GIF tag */")
    header.append(f"#define VUK_{up}_NREG {out_qw}")
    header.append(f"#define VUK_{up}_REGS 0x{regs:x}ull  /* GIF register list */")
    header.append(f"#define VUK_{up}_NIN {item_qw}")
    ins = []
    for k, (iname, fmt, line) in enumerate(inputs):
        usn = fmt.endswith("u") and fmt[:-1] in FORMATS
        base = fmt[:-1] if usn else fmt
        code, nbytes = FORMATS[base]
        header.append(f"#define VUK_{up}_IN_{iname.upper()} {k}  /* {fmt}, {nbytes} bytes */")
        ins.append(f"{{0x{code:x}, {int(usn)}, {nbytes}}}")
    header.append(f"#define VUK_{up}_INPUTS {{{', '.join(ins)}}}  /* UNPACK format, unsigned, bytes */")
    header.append("")
    return gen.lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("-o", "--output", default="-")
    ap.add_argument("-H", "--header", help="write the data memory layout as a C header")
    ap.add_argument("-D", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--entry", action="store_true",
                    help="emit entry functions for VUEntryLowering: llc lays out the data "
                         "memory and puts the layout in section .vu.header; -H then only "
                         "has the constants")
    ap.add_argument("--buffer", type=int, default=512,
                    help="quadwords of VU1 memory per shader batch buffer (default 512: "
                         "double buffered, VIF1 BASE 0, OFFSET 512)")
    args = ap.parse_args()

    try:
        src = open(args.input).read()
        consts_ast, kernels = Parser(lex(src)).program()
        defines = {}
        for d in args.D:
            k, _, v = d.partition("=")
            defines[k] = int(v, 0)
        gen = Gen(dict(defines))
        for cname, e in consts_ast:
            if cname in defines:
                continue  # -D wins
            v = gen.const_eval(e)
            if v is None:
                raise VukError(f"const {cname} is not a compile-time constant")
            gen.consts[cname] = v
        header, funcs = [], []
        for k in kernels:
            if args.entry and k[0] == "shader":
                funcs.append("\n".join(compile_shader_entry(gen, k, args.buffer)))
            elif args.entry:
                funcs.append("\n".join(compile_kernel_entry(gen, k[1:])))
            elif k[0] == "shader":
                funcs.append("\n".join(compile_shader(gen, k, header, args.buffer)))
            else:
                funcs.append("\n".join(compile_kernel(gen, k[1:], header)))
    except VukError as err:
        sys.exit(f"{args.input}: error: {err}")

    out = [f"; generated by vuk from {args.input}"]
    out += gen.globals + [DECLS[u] for u in sorted(gen.uses)] + [""] + funcs + [""] + gen.attrs + gen.meta + [""]
    text = "\n".join(out)
    if args.output == "-":
        sys.stdout.write(text)
    else:
        open(args.output, "w").write(text)
    if args.header:
        guard = re.sub(r"\W", "_", args.header.split("/")[-1]).upper()
        consts = [f"#define VUK_{k} {v}" for k, v in gen.consts.items()]
        if args.entry:
            # the layout: llc's section .vu.header (llvm-objcopy --dump-section)
            base = args.header.split("/")[-1].rsplit(".", 1)[0]
            header.append(f'#include "{base}.layout.h"')
        open(args.header, "w").write(
            f"/* generated by vuk from {args.input} */\n#ifndef {guard}\n#define {guard}\n\n"
            + "\n".join(consts) + "\n\n" + "\n".join(header) + f"\n#endif\n")


if __name__ == "__main__":
    main()
