# FastNoiseLite

Upstream: https://github.com/Auburn/FastNoiseLite

Pinned revision: `7ccfbc16eb1c932568f177d63a9ba51d89bbe516` (v1.1.1), resolved from upstream `refs/tags/v1.1.1` on 2026-10-11. CMake fetches this immutable revision through CPM; it does not consume a moving branch or tag.

`LICENSE` preserves the repository license at this revision. `HEADER-LICENSE.txt` preserves the separate copyright notice in `Cpp/FastNoiseLite.h` (2023, versus 2020 in the repository license). Both are MIT notices. Keep these notices with redistributed software.

Only `Sim.WorldGen.Noise.cpp` includes the upstream header. PPR exports owned numeric settings and an immutable compiled noise field, with no upstream types. This dependency does not establish which procedural algorithm any other game uses.

PPR modifies the private build copy through `cmake/external/PatchFastNoiseLite.cmake`. The script verifies the normalized upstream header's SHA-256 and every replacement count. It makes lattice priming, hash products, neighbour offsets and octave seed increments use unsigned modulo-32-bit arithmetic. Signed hash interpretation and arithmetic right shifts remain intact. This preserves the intended wrapping algorithm without relying on signed-overflow compiler assumptions; the fetched source and all original notices remain unchanged. The generated copy is local to each build directory.
