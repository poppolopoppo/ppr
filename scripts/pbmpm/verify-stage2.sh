#!/usr/bin/env bash
# Stage 2 verification gate:
#   1. emit the full case set into out/stage2
#   2. emit it again into out/stage2-repeat (separate node process)
#   3. compare every file byte-for-byte (timings.json excluded by design)
#   4. verify the frozen stage-1 trace still reproduces
set -euo pipefail
cd "$(dirname "$0")"

node stage2.mjs --out-dir out/stage2
node stage2.mjs --out-dir out/stage2-repeat

node tools/compare-dirs.mjs out/stage2 out/stage2-repeat

node run.mjs --out out/stage1-check.json >/dev/null
cmp out/liquid-k2-s1.json out/stage1-check.json
echo "OK: stage-1 trace out/liquid-k2-s1.json still reproduces byte-for-byte"
rm -f out/stage1-check.json
