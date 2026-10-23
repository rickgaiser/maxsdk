# vuk

A tiny kernel language for VU micro programs. `vuk.py` turns a `.vuk` file into LLVM IR for
`llc -mtriple=vu`, plus a C header with the data memory layout for the EE side.

```
vuk.py poly.vuk -o poly.ll -H poly.h [-D D=8]
llc -mtriple=vu -mcpu=vu1 -O3 -filetype=obj poly.ll -o poly.o
```

## Example

```
const D = 16;

kernel poly(n: i16, c: f32[D + 1], x: f32x4[]) {
    for i in 0..n {
        var v = x[i];
        var y = c[D] * v + c[D - 1];
        for k in 2..D + 1 {
            y = y * v + c[D - k];
        }
        x[i] = y;
    }
}
```

## Grammar

```
program  = { "const" NAME "=" expr ";" | kernel }
kernel   = "kernel" NAME "(" [ param { "," param } ] ")" block
param    = NAME ":" type
type     = "i16" | "f32" | "f32" "[" expr "]" | "f32x4" "[" [ expr ] "]"
block    = "{" { stmt } "}"
stmt     = "var" NAME "=" expr ";"
         | target "=" expr ";"
         | "for" NAME "in" expr ".." expr block
target   = NAME | NAME "[" expr "]"
expr     = term { ("+" | "-") term }
term     = unary { ("*" | "/") unary }
unary    = "-" unary | NUMBER | NAME | NAME "[" expr "]" | "(" expr ")"
```

`//` starts a comment.

## Meaning

- **Values.** `f32x4` is four floats, one per VU lane. An `f32` value is broadcast to all four
  lanes. `i16` is a VU integer (VI) register, used for counts and indices.
- **Parameters.** A micro program takes no arguments, so parameters are places in VU data
  memory, laid out in order from quadword 0. Each parameter starts at a new quadword:
  - `i16`: field x of one quadword.
  - `f32`: field x of one quadword.
  - `f32[K]` (a table): K floats, 4 per quadword. Index it with a constant only.
  - `f32x4[K]`: K quadwords. `f32x4[]` (no size) is allowed only as the last parameter.

  `-H` writes these offsets as `VUK_<KERNEL>_<PARAM>` defines, plus every `const` as `VUK_<NAME>`.
  The EE side writes the inputs there and starts the program.
- **Scalars and tables are read once**, at the start of the kernel. Only `f32x4` arrays are
  read and written in the loops.
- **Loops.** `for i in a..b` runs `i` from `a` up to `b - 1`. With constant bounds the loop is
  unrolled at compile time, and `i` is a constant (usable as a table index). Otherwise it is a
  run-time loop over an `i16`.
- **Constants.** `const NAME = expr;` defines a compile-time integer. `-D NAME=VALUE` on the
  command line overrides it.

## Not in the language (yet)

Branches, reductions, function calls, integer division, VU1-only instructions (XGKICK).
