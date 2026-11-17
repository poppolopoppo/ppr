//-----------------------------------------------------------------------------
// NOT an upstream file — resolution shim added by the vendoring step.
//
// Upstream src/sim.js is vendored byte-identical and imports its siblings with
// relative specifiers, including `import * as gpu from "./gpu.js"`. This file
// exists only so that specifier resolves to the compute-only replacement at
// scripts/pbmpm/src/gpu.js. Upstream's own src/gpu.js (DOM, canvas, render
// pipeline) is deliberately not vendored.
//
// `export *` forwards the replacement module's live bindings, and the relative
// specifier below resolves to the same module instance the runners import, so
// the mutable gpuContext stays a single object.
//-----------------------------------------------------------------------------

"use strict";

export * from "../../src/gpu.js";
