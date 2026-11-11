# Document-only comprehension benchmark

Date: 2026-11-10
Reviewer model: `opencode/deepseek-v4.1-flash-free`
Child session: `ses_e38d82ec5ffe5nRBEhBo0yGdf5`
Source: [current design draft](2026-11-10-oni-port-design.md), 1,067 lines at review time.

## Method

The owner requested a fresh DeepSeek Flash Free sublane with the plan as its sole
context. The current artifact is a design draft, not yet an implementation plan.
The child received only that document's path and a comprehension-review task;
no conversation summary, decision answer key, linked reference documents or source
code was supplied. It reported using only the named document. Normal inherited
harness instructions still apply; this was task-context isolation, not a claim
that the model received no system instructions.

Round one required a teach-back, decision-status ledger, frame timeline,
scenario analysis and ranked ambiguity report. Round two asked it to recheck
questionable conclusions using the same document, and solve concrete scheduling,
particle-selection and extent examples. No corrected design answers were supplied
in that follow-up. The complete responses are retained in the child session.

This record is the coordinator's audited summary, not a verbatim transcript or a
new design authority. No production edits, builds, captures or visual validation
were performed. The design itself was not changed during this benchmark.

## Result

**The central approved decisions are understandable from the document alone.**
The reviewer recovered:

- Cleanup and preservation first, followed by owner acceptance before lighting
  integration; only the owner accepts visual quality.
- The 57 MiB subtotal and its exclusions, 8-byte coarse intervals, signed 32-byte
  CH, packed material-ID/FP16-temperature cells and 8-byte source materials.
- Cached isotropic outgoing radiance with diffuse-only feedback, and a single
  in-place CH field with source reads before fine writes before scene reads.
- Preserved visible PBR, thermal emission on opaque surfaces only, independent
  visibility classification, particle precedence and separate receive-only gas.
- Coarse quarter-probe cadence, retained direction identity, dark cold start,
  active-presentation-time accumulation and submission-based state commits.
- Always-on Hable reconstruction, unexposed screen history, independent post
  processing, retained controls and arbitrary-size padding/cropping.
- The required map cases, real-window automation and profiler automation, and
  the distinction between requirements, proposals and unverified operation.

Concrete follow-up calculations were correct:

- With n=0 and h=2, only k=2 is initialized; other probes remain invalid.
- With n=5 and h=2, k=0,1,2,3 last updated at frames 2,3,4,5 respectively.
- L1 selects 16,384 probes with 16 directions; L5 selects 64 with 4,096
  directions. Each updates 262,144 interval records, not a proven traversal count.
- At 1921x1081 with P=2: content 961x541, raster 481x271, backing 962x542,
  enlarged content 1922x1082, cropped output 1921x1081. Backing padding is
  excluded; the partial final content block remains content.
- Without terrain, opaque metal beats a nearer receive-only transparent particle;
  same-class equal-distance ties use the smaller stable ID. Gas is separate.

## Reviewer errors exposed by the follow-up

The first pass was not error-free. It corrected these after targeted re-reading:

- Opaque black guarantees blocking, not zero source: authored and thermal
  emission remain independent terms.
- Excluded transparent/gas visible-only thermal glow is already disallowed by
  the current scope; it is not an unresolved feature choice.
- Updated interval records are not necessarily traversal calls. Coincidence
  with a retired ray-cap number is not a contradiction or performance evidence.
- The approved CH record and rejected per-material PBR record are clearly
  different records, despite both being 32 bytes.
- Temperature spacing is 2 K near 3000 K and 4 K near 6000 K; its first ledger
  incorrectly abbreviated those reference temperatures.

Coordinator caution: the follow-up occasionally overcorrected into stronger
claims than the draft supports. In particular, do not treat its assertion of a
single acceptance event for lighting and screen reconstruction as a newly fixed
submission API. The draft defines commits for work actually submitted; concrete
submission grouping remains to specify. Likewise, a reset-condition list does
not by itself define diagnostic-view routing through temporal history. Those
details must be written down rather than inferred from the reviewer's confidence.

## Genuine documentation defects

1. Sections 1, 2 and the section 4 heading still call representation/accumulation
   reopened or direct a comparison before selection, despite later explicit
   approval of order-four CH, its encoding and accumulation policy.
2. The data table's blanket "Proposed representation / count" header mixes
   approved layouts with explicitly unfinished scratch and screen resources.

These stale status labels should be reconciled without reopening approved choices.

## Remaining specification work confirmed

The reviewer correctly distinguished the following from decisions already made:

- Exact angular sequence, hash, CH reconstruction/normalization and
  roughness-dependent visible specular sampling.
- Temperature publication, LUT numerics/radiance normalization and defined
  invalid/out-of-range handling.
- Winner-selection scratch and publication synchronization; material ABI,
  sample-rate shading, screen resources and padded projection formulas.
- Detailed exposure/resolve integration, map parameters, diagnostic controls,
  display-exercise cases and profiler acquisition/export contracts.

**Verdict:** useful comprehension pass for the accepted architecture, with
demonstrated reviewer mistakes that required audit. The draft is not yet a
complete implementation handoff. This benchmark establishes neither algorithmic
correctness nor GPU performance, operational readiness or visual acceptance.
