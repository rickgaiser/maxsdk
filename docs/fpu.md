# FPU (COP1) and the VU multipliers

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| The multiplier is **not commutative**: with a power of two in fs and low mantissa bits in ft, `mul.s`, `vmul` and `vmula` give a product 1 ULP low; the other order is exact | 1 ULP | single precision, normal numbers | `bench/vudemo/mulprobe.c` | 2026-10-23 |
| No NaN/INF handling: INF and NaN bit patterns are ordinary numbers (exponent 255) | 19 of 30 IEEE special cases differ | add/sub/mul/div/sqrt | `bench/IEEE754.md` | 2026-10 |
| Exponent overflow saturates to `0x7FFFFFFF` / `0xFFFFFFFF` (±max with all mantissa bits), not ±INF | | e.g. INF+INF, INF*1e6, 1/0, 0/0 | `bench/IEEE754.md` | |
| INF − INF and INF × 0 give 0 (not NaN) | | | `bench/IEEE754.md` | |
| `div.s` treats INF as max_float: INF/1e6 = 3.40282e32, INF/INF = 1.0 | | | `bench/IEEE754.md` | |
| Mantissa overflow without exponent overflow clamps to ±max_float | | | `bench/IEEE754.md` | |

Consequences: math tests compare within ULP limits (the VU demo uses 4 ULP), and FP code must not rely
on INF/NaN. **Manual:** the EE Core User's Manual describes the clamping, not the multiplier asymmetry.

Latencies of FPU instructions are in [ee-core.md](ee-core.md#fpu-timing).
