# Selected-fork PB-MPM compute port

The numerical authority is [ErikSom/pbmpm at
859c4490c749c1cc364d099543dbf5aa0bd9be1b](https://github.com/ErikSom/pbmpm/tree/859c4490c749c1cc364d099543dbf5aa0bd9be1b).
The original BSD-3-Clause notice is in `LICENSE.md`. Historical EA pins remain
provenance, not alternative production backends.

`solver.slang` has two production entry points, `fused0` and `fused1`,
instantiating the same fused body with compile-time iteration0/1. Both retain
the node contact/error stage and shared barriers; only the existing gather/
integration selectors become compile-time constants. The host still encodes
two immutable roots and two dispatches per substep. `sparse_grid.slang`
discovers compact buckets/pages, counts and inserts rows, and initializes the
destination through separately ordered GPU passes. Each substep constructs
buckets from current enabled rows and dispatches fused iteration0 then iteration1 indirectly.
Iteration0 bootstraps scatter without gathering; iteration1 gathers, integrates
and scatters. The engine supports K2 only and rejects all other counts before
GPU work. Production advances eight evolving dt=1/480 substeps per fixed tick;
drawing does not advance simulation. Each fused dispatch owns its immutable
selector and predicted-body-base constants through command completion.

`fork_core.slang` supplies shared64/32 storage access and selected-fork SVD,
constraint and integration arithmetic. `types.slang` supplies scalar column
algebra, shapes and quadratic weights. Arithmetic expression forms and operation
order are retained from the conformance port. The production-only insertion
sites preserve nonfinite endpoint rejection, terrain/body contact before the
guardian and the six-word first-error protocol. The no-hook conformance path
keeps its separate three-word sink.

Two sparse page payload pools and one shared absolute-budget pool replace the
dense production node arrays. Canonical 8-by-8 pages preserve the legacy flat
node key, including its right-edge alias, and the original contact coordinate
tuple. A dispatch reads its predecessor's source: previous A survives iteration0,
then the current page list remaps A for iteration1. Tick-start A is logically
empty. Each destination and its budget are initialized before scatter; absent
source pages return zero words. Shared tile storage is explicitly zeroed and
all lanes cross the shared barriers.


Bucket addressing converts each already-validated nonnegative position to its
integer cell before unsigned division by6. This preserves the mathematical
partition required by `floor(position)-1` and its3-by-3 stencil; a floating
reciprocal near a six-cell boundary can select the next bucket prematurely.
Both production stencil traversals check their signed2D local coordinates
before flattening or shared access and reject a malformed candidate through the
existing structural-error sink. No valid contribution is clamped or omitted.
The pinned WGSL uses floating bucket division and assumes this support invariant;
this is a native discrete-address correction, not a PB-MPM equation change.
The failure probe proved local word index-16; exact failing substep position and
compiler lowering were not captured, so those remain unobserved.

Buckets use six-cell tiles and64-row records. Per-storage row and bucket
capacities and sparse pools are prepared transactionally for the actual admitted
row count with both hot/cold row buffers, before publication. Sparse admission
is independent of spare row-buffer capacity. Bounded atomic reservation rejects overflow rather than clamping
or issuing an out-of-range indirect dispatch. Stable rows are only permuted in
the bucket index buffer; particle storage/order and metadata identity stay fixed.

The device ABI is a64-byte hot row plus a24-byte cold row. The saved semantic
record remains22 fields/88 bytes, packed by field name; no v5 codec change.
All evolving fields retain FP32 storage. Terrain masks use4 bits per cell,
boundary tile occupancy uses1 bit per8x8 tile, and the exact contour tables use
8 bytes per case plus one4-byte word for all8 contour points.
CPU admission retains `enabled==1`;
the fork's disabled-row branch is unreachable in production. Every saved row
passes complete semantic validation. Ordinary completion reads only the24-byte
status; full committed readback requires an explicit frozen-epoch snapshot.

The numerical branches include liquid, elastic, sand and visco, guarded zero-pair
SVD, liquid density floor0.05, singular floor0.1, source volume lerp and unilateral
guardian contact with fixed tangential friction0.5. Per-field unsigned CAS
absolute-sum budgets prove signed scatter reductions stay within INT_MAX.
Each contribution keeps its original conversion and rounding, then reserves its
absolute encoded magnitude in a workgroup-local budget before the signed shared
addition. After the shared barrier, each nonempty node/field reserves its complete
local absolute total against the same cross-group sparse budget before merging
the signed sum; a zero signed sum still consumes its nonzero absolute total.
Global rejection skips that signed merge. Two256-word shared uint arrays retain
the absolute totals and actual contributing-row witnesses, adding2048 bytes for
4096 total shared bytes per64-thread group. First-error race ordering may change
which contributing row is reported; the cause/node/field/valid-row contract stays.
Nonfinite and scatter conversion/overflow errors reject the entire candidate.
The four nonaccumulating post-contact NODE stores follow WGSL2026-10-21
section15.7.6: scale once, reject nonfinite, then clamp/truncate to the
f32-representable i32 endpoints[-2147483648,2147483520]. This approved source-language
conversion exception does not change scatter guards or feed INT_MIN into signed
absolute-sum accounting.
Old v5 semantic fields still load exactly; subsequent trajectories follow this
replacement solver implementation.

Terrain and one-way predicted bodies are engine adapters. Grid contact runs
after literal source shapes and before the source guardian; particle endpoint
contact runs after source shapes and before endpoint clamping. Grid moving-body
contact projects relative to local boundary displacement; particle endpoints
retain direct geometric penetration subtraction. `environment.slang` preserves the
complete contour search and explicit incomplete-boundary errors. No reciprocal
reaction readback or ordinary per-particle CPU loop is introduced.

`fork_conformance.slang` retains the explicitly test-only dense storage route and
full frozen-WGSL stage/grid comparison. It is not a production fallback.
Production parity observes stages1,3,...,15, tick2 and restored22, using the same
fixed batch5 word bounds. The production fixture projects out the oracle's
disabled row while preserving active row order and original-index comparisons;
the full conformance fixture retains disabled-row coverage. Production exposes
no grid readback. No claim of full material fidelity follows from the two oracle
scenarios. `contact_checks.slang` is test-only inspection of the production grid
contact helper; it is not another solver or product entry. `source_checks.slang`
inspects the actual shared algebra/validation helpers.

Build, conformance, production parity, contact and lifetime gates must run after
independent adapter/root review; source editing alone is not validation evidence.
