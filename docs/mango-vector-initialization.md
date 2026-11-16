# Mango SIMD vectors: initialisation and comparison traps

`mango::math::Vector<T, N>` (used as `float2`, `float3`, `float4`, `int2`, `int3`,
`int4`, and the wider forms) is **not** a value-initialising type. Its default
constructor is declared `explicit Vector() {}` and **leaves every component
indeterminate**. This has caused real defects in this repository, three of them in
a single feature:

- `InteriorRegion::m_origin` / `m_extent` initialised `{}`, so the "canonical empty"
  rectangle carried stack garbage and the border kernel skipped a stale area;
- `ReconstructionParameters` extents and jitter initialised `{}`, reading
  indeterminate frame geometry;
- CH quantisation helpers left coefficients 3–8 uninitialised, so a test oracle
  compared against garbage;
- plus a hard compile failure: `std::array<float3, 9> values{}` does not compile
  because aggregate value-initialisation needs a non-explicit default constructor.

## Rules

1. **Never value-initialise a Mango vector.** `float3 v{}`, `int2 p{}` and
   `std::array<float3, N> a{}` are all wrong. Write the components:
   `float3 v{0.0f, 0.0f, 0.0f}`, `int2 p{0, 0}`, or use `zero_v` / an existing named
   constant. This applies to struct members, locals, arrays and aggregates.
2. **For arrays of vectors**, either default-initialise and assign every element
   (`std::array<float3, 9> values;` followed by an assignment for each index), or
   brace-initialise with explicit vectors. Do not mix: a partially assigned array
   of vectors is indeterminate where it was not written.
3. **Never default a comparison operator on a type containing a vector.**
   `bool operator==(...) const = default;` instantiates Mango's `compare_eq`, which
   does not compile for these types. Write the component-wise comparison:
   `a.m_offset.x == b.m_offset.x && a.m_offset.y == b.m_offset.y`.
4. **Initialise before reading, not before use.** A struct with a vector member that
   is only meaningful in one state (an "empty" or "unset" variant) still needs an
   explicit zero value for that state, because the whole object is copied and
   compared before the state is inspected.
5. **Treat `{}` on any Mango type in review as a defect**, including inside tests and
   probes: an uninitialised test oracle produces a comparison that cannot fail, or
   fails for the wrong reason.

## Quick reference

| Intent | Wrong | Right |
| --- | --- | --- |
| zero vector | `float3 v{}` | `float3 v{0.0f, 0.0f, 0.0f}` / `zero_v` |
| zero int vector | `int2 p{}` | `int2 p{0, 0}` |
| array of vectors | `std::array<float3, 9> a{}` | `std::array<float3, 9> a;` then assign all, or list all explicitly |
| equality | `operator==(...) = default;` | compare components explicitly |

## Why it is easy to miss

For `std::array<float, N>` and ordinary aggregates, `{}` is the idiomatic zero
initialiser and the compiler enforces completeness. Mango's `explicit` default
constructor breaks that expectation silently for members (no diagnostic) and loudly
for arrays (a confusing "explicit constructor in copy-initialization" error). The
failure mode is therefore *indeterminate data*, not a compile error, unless the value
happens to be used by an array initialiser.
