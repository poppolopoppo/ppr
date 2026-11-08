# Hable MSAA spatial prerequisite

`UpsamplingViaMultisampling.hlsl` is the unmodified, MIT-licensed John Hable
download linked by [Upsampling via Multisampling](https://filmicworlds.com/blog/upsampling-via-multisampling/),
retrieved 2026-11-01 from
https://filmicworlds.com/downloads/2025_05_05_msaa_upsampling/UpsamplingViaMultisampling.hlsl.
SHA256: `fcdf0482a5c982d248e0656c2001b579d0686e4acbd4574dd253a230f00ed0cd`.
The original bytes, including their legacy quotation encoding, are retained.
LICENSE normalizes only those quotation marks to ASCII.

`assets/shaders/spatial_msaa4.slang` adapts its 4x spatial sample grid with the
continuous gradient weights described in Hable's later
[Temporal Super Resolution via Multisampling](https://filmicworlds.com/blog/temporal-super-resolution-via-multisampling/).
There is no history, colour clamp, jitter, motion or temporal blend in this batch.
No dependency on an upstream engine is introduced.

## Deliberate correction and independently derived invariants

The source repeats two taps in each lower output bucket. The adaptation instead
averages the four distinct points of each stated 2x2 grid square:

| Output (x,y) | Source grid (row,column) | Source-relative centre |
|---|---|---|
| (0,0) | (4,4),(4,5),(5,4),(5,5) | (1/4,1/4) |
| (1,0) | (4,6),(4,7),(5,6),(5,7) | (3/4,1/4) |
| (0,1) | (6,4),(6,5),(7,4),(7,5) | (1/4,3/4) |
| (1,1) | (6,6),(6,7),(7,6),(7,7) | (3/4,3/4) |

Grid coordinate `g` maps to source-relative coordinate `(g-3.5)/4`.
The four known sample coordinates are (column,row) (5,4),(7,5),(4,6),(6,7).
Each diamond is a convex combination of two endpoint means. For affine RGB,
every horizontal/vertical pair spans four grid units, so their luminance
differences and epsilon-floored weights are common throughout the stencil.
Individual midpoint-position biases cancel within each of the four correct
bucket sums. Thus constants have unit weight and interior affine first moments
are exactly the four centres above. Border replication intentionally changes
those first moments; border results use the separately constructed CPU oracle.

The double CPU oracle discovers row/column neighbours geometrically in a cross
of known samples; it does not copy shader tap names or its bucket expressions.
Independent analytic affine and constant assertions additionally constrain it.

## Admission and remaining integration

The test first recovers indexed positions using real threshold rectangles and
per-index multisample loads. The expected layout comes from the selected method,
not an assumed backend table; an index permutation is accepted. This bounded
probe qualifies the tested pixels/device/backend, not every possible device or
pixel. Sixteen midpoint thresholds give nominal coordinate brackets of width
1/16 (not an exact coordinate query); rasterizer subpixel quantization must also
be considered before treating them as strict geometric bounds. A future runtime
admission contract must establish the exact pattern guarantee. A second
synthetic-data run imposes a different permutation to stress shader indexing;
it does not claim the hardware changed sample locations.
The shader requires an admitted permutation and exact 2x extents. It preserves
signed finite linear HDR RGB and writes opaque composite alpha 1, not an opacity
or depth history. Inputs must already contain correctly composed sample colours.

The ordinary game and RetroPass remain unchanged. Real contour sample coverage,
runtime admission, temporal reconstruction, resize/submission integration and
4K cost qualification remain separate work. No FPS claim follows from this batch.
