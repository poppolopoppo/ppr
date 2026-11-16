#!/usr/bin/env bash
# Determinism gate: run the stage-1 trace twice in two processes and compare bytes.
set -euo pipefail
cd "$(dirname "$0")"

node run.mjs --out out/run-a.json
node run.mjs --out out/run-b.json

cmp out/run-a.json out/run-b.json
echo "OK: out/run-a.json and out/run-b.json are byte-identical"
sha256sum out/run-a.json out/run-b.json
