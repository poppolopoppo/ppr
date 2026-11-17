//-----------------------------------------------------------------------------
// Stage 2 driver: emit a comparable trace set for the EA PB-MPM demo.
//
//   node stage2.mjs                      # 4 families x K{2,5,10} x 8 substeps
//   node stage2.mjs --steps 12           # longer series
//   node stage2.mjs --families sand --iterations 2
//
// Every case advances N *simulated* substeps and snapshots the particle buffer
// and all three grid buffers after each one, inside the frame's own command
// buffer (see src/gpu.js frameCopies). The upstream quirk that the first
// substep of every sim.update() call dispatches no g2p2g work is handled by
// advancing exactly one simulated substep per update() call and proving, per
// dispatch, the workgroup count through the indirect-args probes.
//-----------------------------------------------------------------------------

"use strict";

import {createRequire} from "node:module";
import {readdirSync, readFileSync, writeFileSync, mkdirSync} from "node:fs";
import {createHash} from "node:crypto";
import {execFileSync} from "node:child_process";
import {dirname, join, relative} from "node:path";
import {fileURLToPath} from "node:url";

// ------------------------------------------------------------------
// configuration
// ------------------------------------------------------------------

const HARNESS_DIR = dirname(fileURLToPath(import.meta.url));

// Vendored upstream checkout; see run.mjs and upstream/README.md.
const VENDORED_UPSTREAM_DIR = join(HARNESS_DIR, "upstream");
const UPSTREAM_DIR = process.env.PBMPM_UPSTREAM ?? VENDORED_UPSTREAM_DIR;
const UPSTREAM_COMMIT = "61749b6c2d2631ed243f86882752ba43196f2de7";
const UPSTREAM_COMMIT_SUBJECT = "Fix rendering smoothstep function in new version of Dawn";

const args = process.argv.slice(2);
function argValue(name, fallback)
{
    const index = args.indexOf(name);
    return index === -1 ? fallback : args[index + 1];
}

function argList(name, fallback)
{
    const index = args.indexOf(name);
    return index === -1 ? fallback : args[index + 1].split(",");
}

const OUT_DIR = argValue("--out-dir", join(HARNESS_DIR, "out", "stage2"));
const STEP_COUNT = Number(argValue("--steps", "8"));
const FAMILY_FILTER = argList("--families", ["liquid", "elastic", "sand", "visco"]);
const ITERATION_FILTER = argList("--iterations", ["2", "5", "10"]).map(Number);

// Upstream renders on requestAnimationFrame; the harness steps time explicitly.
const FRAME_DELTA_MS = 1000.0 / 60.0;

// Domain: 32x32 grid, 256x256 render resolution (simResDivisor 8).
const GRID_SIZE = [32, 32];
const SIM_RES_DIVISOR = 8;
const RESOLUTION = [GRID_SIZE[0] * SIM_RES_DIVISOR, GRID_SIZE[1] * SIM_RES_DIVISOR];

// Shared seed block: 8x8 in-domain cells at cell centre, one particle per cell.
const BLOCK_ORIGIN = [8, 8];
const BLOCK_SIZE = [8, 8];
const BLOCK_CENTER = [
    BLOCK_ORIGIN[0] + BLOCK_SIZE[0] / 2,
    BLOCK_ORIGIN[1] + BLOCK_SIZE[1] / 2,
];

// Shared initial velocity field (grid cells per substep), identical for every
// family so the four traces are comparable. It is linear in position, so the
// G2P displacement gradient is ~constant over the block, and it combines
//   - isotropic expansion  k * (p - c)   -> non-zero tr(dF), i.e. volume change
//   - shear                a * (p.y - c.y) in x -> non-zero deviatoric dF
// Both are needed: expansion drives Sand's logJp path, shear drives the
// deviatoric stress that Visco's plasticity clamps.
const EXPANSION_RATE = 0.02;
const SHEAR_RATE = 0.30;

// Families: material id and colour from src/sim.js SimEnums and
// shaders/particleEmit.wgsl addParticle().
const FAMILIES = [
    {id: "liquid", material: 0, enumName: "MaterialLiquid", color: [0.2, 0.2, 1.0]},
    {id: "elastic", material: 1, enumName: "MaterialElastic", color: [0.2, 1.0, 0.2]},
    {id: "sand", material: 2, enumName: "MaterialSand", color: [1.0, 1.0, 0.2]},
    {id: "visco", material: 3, enumName: "MaterialVisco", color: [1.0, 0.5, 1.0]},
    {id: "mixed", material: null, enumName: "LiquidSandElastic"},
];

// ------------------------------------------------------------------
// webgpu runtime
// ------------------------------------------------------------------

const require = createRequire(import.meta.url);
const {create, globals} = require("webgpu");

for(const key of Object.keys(globals))
{
    globalThis[key] = globals[key];
}

// Harness instrumentation (same contract as run.mjs): add COPY_SRC to storage
// buffers so buffers sim.js creates as STORAGE|COPY_DST can be snapshotted, and
// keep a per-frame registry of created buffers.
const createdBuffers = [];
const originalCreateBuffer = GPUDevice.prototype.createBuffer;
GPUDevice.prototype.createBuffer = function(descriptor)
{
    let patched = descriptor;
    if(descriptor && typeof descriptor.usage === "number" && (descriptor.usage & GPUBufferUsage.STORAGE))
    {
        patched = {...descriptor, usage: descriptor.usage | GPUBufferUsage.COPY_SRC};
    }

    const buffer = originalCreateBuffer.call(this, patched);
    createdBuffers.push(buffer);
    return buffer;
};

globalThis.fetch = async (url) =>
{
    const relativePath = String(url).replace(/^\.\//, "");
    const shaderText = readFileSync(join(UPSTREAM_DIR, relativePath), "utf8");
    return {ok: true, status: 200, async text() {return shaderText;}};
};

const sim = await import("./upstream/src/sim.js");
const gpu = await import("./src/gpu.js");

function fail(message)
{
    console.error(`FATAL: ${message}`);
    process.exit(1);
}

function lastBufferLabelled(label)
{
    for(let i = createdBuffers.length - 1; i >= 0; --i)
    {
        if(createdBuffers[i].label === label)
        {
            return createdBuffers[i];
        }
    }
    return null;
}

// ------------------------------------------------------------------
// inputs
// ------------------------------------------------------------------

function buildInputs(iterationCount)
{
    return {
        gridSize: GRID_SIZE,
        resolution: RESOLUTION,
        simResDivisor: SIM_RES_DIVISOR,

        timeStamp: 0,
        doReset: true,
        doPause: false,

        isMouseDown: false,
        mousePosition: [RESOLUTION[0] / 2, RESOLUTION[1] / 2],
        mousePrevPosition: [RESOLUTION[0] / 2, RESOLUTION[1] / 2],
        mouseRadius: 100,
        mouseFunction: sim.SimEnums.MouseFunctionGrab,

        simRate: 120,
        iterationCount,
        particlesPerCellAxis: 1,
        rhoZeroMultiplier: 1.0,
        fixedPointMultiplierExponent: 7,
        useGridVolumeForLiquid: 1,
        gravityStrength: 2.5,
        liquidViscosity: 0.01,
        liquidRelaxation: 1.5,
        elasticRelaxation: 1.5,
        elasticityRatio: 1.0,
        frictionAngle: 30,
        plasticity: 0.9,
        borderFriction: 0.5,

        // No SDF shapes: constructShapeBuffer emits one inert filler SimShape,
        // and shapeCount (inputs.shapes.size) is 0.
        shapes: new Set(),
    };
}

function describeInputs(inputs, family, stepCount)
{
    return {
        gridSize: inputs.gridSize,
        resolution: inputs.resolution,
        simResDivisor: inputs.simResDivisor,
        simRate: inputs.simRate,
        deltaTime: 1.0 / inputs.simRate,
        iterationCount: inputs.iterationCount,
        particlesPerCellAxis: inputs.particlesPerCellAxis,
        rhoZeroMultiplier: inputs.rhoZeroMultiplier,
        fixedPointMultiplierExponent: inputs.fixedPointMultiplierExponent,
        fixedPointMultiplier: Math.ceil(Math.pow(10, inputs.fixedPointMultiplierExponent)),
        useGridVolumeForLiquid: inputs.useGridVolumeForLiquid,
        gravityStrength: inputs.gravityStrength,
        liquidViscosity: inputs.liquidViscosity,
        liquidRelaxation: inputs.liquidRelaxation,
        elasticRelaxation: inputs.elasticRelaxation,
        elasticityRatio: inputs.elasticityRatio,
        frictionAngle: inputs.frictionAngle,
        plasticity: inputs.plasticity,
        borderFriction: inputs.borderFriction,
        mouseRadius: inputs.mouseRadius,
        mouseFunction: inputs.mouseFunction,
        isMouseDown: inputs.isMouseDown,
        shapeCount: inputs.shapes.size,
        material: family.material,
        materialName: family.enumName,
        steps: stepCount,
    };
}

function provenance()
{
    return {
        gridSize: "harness decision (32x32); upstream derives floor(canvas/simResDivisor) in ui.windowResize",
        resolution: "harness decision = gridSize * simResDivisor (256x256)",
        simResDivisor: "ui.js g_uiElements default 8",
        simRate: "harness decision 120 (ui.js default 240); with a 60 Hz frame delta one update() call is 2 substeps, of which exactly one simulates",
        iterationCount: "case parameter (2/5/10); ui.js default is 5, damBreak.json overrides 10",
        particlesPerCellAxis: "harness decision 1 (ui.js default 2)",
        rhoZeroMultiplier: "harness decision 1.0; not a declared SimConstants member so it never reaches the GPU",
        fixedPointMultiplierExponent: "ui.js g_uiElements default 7",
        useGridVolumeForLiquid: "ui.js g_uiElements default true (1)",
        gravityStrength: "ui.js g_uiElements default 2.5",
        liquidViscosity: "ui.js g_uiElements default 0.01",
        liquidRelaxation: "ui.js g_uiElements default 1.5",
        elasticRelaxation: "ui.js g_uiElements default 1.5",
        elasticityRatio: "ui.js g_uiElements default 1",
        frictionAngle: "ui.js g_uiElements default 30",
        plasticity: "ui.js g_uiElements default 0.9 (Visco yield surface exp(1 - plasticity) = 1.10517)",
        borderFriction: "ui.js g_uiElements default 0.5",
        mouseRadius: "main.js g_mouseRadius = 100 render px; inactive",
        initialVelocity: `harness decision, shared by all four families: v = (${EXPANSION_RATE}*(x-cx) + ${SHEAR_RATE}*(y-cy), ${EXPANSION_RATE}*(y-cy)) cells/substep around c = (${BLOCK_CENTER.join(", ")})`,
        initialVelocityRationale: "from rest the flow is a uniform free fall, so dF stays exactly 0 and no material law can be exercised within 8 substeps; a fixed shared velocity field makes every law observable while keeping all four families comparable. Expansion (tr(dF) > 0) is what drives Sand's logJp path; shear (deviatoric dF) is what gives Visco an anisotropy to clamp. Rate chosen from a sweep (0.02..0.4) so that Visco reaches its yield surface at K = 2, 5 and 10 within 8 substeps",
        logJp: "0 for every family (the Sand seed requires logJp = 0 = no carried dilation; the field is unused by the other three, so it is kept identical)",
    };
}

// ------------------------------------------------------------------
// deterministic seed
// ------------------------------------------------------------------

function velocityAt(position)
{
    const dx = position[0] - BLOCK_CENTER[0];
    const dy = position[1] - BLOCK_CENTER[1];

    return [
        EXPANSION_RATE * dx + SHEAR_RATE * dy,
        EXPANSION_RATE * dy,
    ];
}

function buildSeed(family)
{
    const rows = [];
    const decoded = [];

    for(let y = 0; y < BLOCK_SIZE[1]; ++y)
    {
        for(let x = 0; x < BLOCK_SIZE[0]; ++x)
        {
            // 16 liquid + 16 sand + 4 elastic, in row-major order. Adjacent
            // centres are one cell apart; quadratic support radius is 1.5,
            // so both material interfaces share nonzero-weight grid nodes.
            let particleFamily = family;
            if(family.id === "mixed")
            {
                if(y < 4)
                    particleFamily = FAMILIES[x < 4 ? 0 : 2];
                else if(y < 6 && x >= 1 && x < 3)
                    particleFamily = FAMILIES[1];
                else
                    continue;
            }

            const position = [BLOCK_ORIGIN[0] + x + 0.5, BLOCK_ORIGIN[1] + y + 0.5];
            const displacement = velocityAt(position);

            const particle = {
                position,
                displacement,
                deformationGradient: [1, 0, 0, 1],
                deformationDisplacement: [0, 0, 0, 0],
                liquidDensity: 1.0,
                mass: 1.0,
                material: particleFamily.material,
                volume: 1.0,
                lambda: 0.0,
                logJp: 0.0,
                color: particleFamily.color,
                enabled: 1.0,
            };

            rows.push(particle);
            decoded.push({position, displacement, material: particleFamily.material});
        }
    }

    return {rows, decoded};
}

// ------------------------------------------------------------------
// case runner
// ------------------------------------------------------------------

async function runCase(runtime, family, iterationCount, stepCount)
{
    const {device, gpuContext} = runtime;
    const startedAt = performance.now();

    const inputs = buildInputs(iterationCount);
    gpu.resetBuffers(inputs.gridSize);

    const seed = buildSeed(family);
    gpu.seedParticles(seed.rows, seed.rows.length);
    gpuContext.expectedSubstepIterations = iterationCount;

    const particleCount = seed.rows.length;
    const particleBytes = particleCount * gpu.ParticleByteStride;
    const gridBytes = GRID_SIZE[0] * GRID_SIZE[1] * 16;
    const gridStepBytes = 3 * gridBytes;
    const probesPerFrame = 2 * iterationCount + 4;

    const staging = {
        particles: device.createBuffer({
            label: "stage2ParticleStaging",
            size: stepCount * particleBytes,
            usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
        }),
        grid: device.createBuffer({
            label: "stage2GridStaging",
            size: stepCount * gridStepBytes,
            usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
        }),
        bukkit: device.createBuffer({
            label: "stage2BukkitArgsStaging",
            size: stepCount * 16,
            usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
        }),
        probes: device.createBuffer({
            label: "stage2ProbeStaging",
            size: stepCount * probesPerFrame * 16,
            usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
        }),
    };

    gpuContext.probeTarget = staging.probes;
    gpuContext.probeOffset = 0;

    const frameRecords = [];

    for(let step = 0; step < stepCount; ++step)
    {
        createdBuffers.length = 0;

        gpu.beginFrame();

        const frameInputs = {
            ...inputs,
            timeStamp: step * FRAME_DELTA_MS,
            doReset: true,
        };

        device.pushErrorScope("validation");
        sim.update(gpuContext, frameInputs);

        // sim.js created this frame's grid and bukkit buffers during update().
        const gridBuffers = createdBuffers.filter((buffer) => buffer.label?.startsWith("gridBuffer")).slice(-3);
        if(gridBuffers.length !== 3)
        {
            fail(`step ${step}: expected 3 grid buffers, found ${gridBuffers.length}`);
        }

        const bukkitDispatchBuffer = lastBufferLabelled("bukkitParticleDispatchBuffer");
        if(!bukkitDispatchBuffer)
        {
            fail(`step ${step}: bukkit dispatch buffer not found`);
        }

        gpuContext.frameCopies = [
            {src: gpuContext.particleBuffer, dst: staging.particles, dstOffset: step * particleBytes, size: particleBytes},
            ...gridBuffers.map((buffer, index) => ({
                src: buffer,
                dst: staging.grid,
                dstOffset: step * gridStepBytes + index * gridBytes,
                size: gridBytes,
            })),
            {src: bukkitDispatchBuffer, dst: staging.bukkit, dstOffset: step * 16, size: 16},
        ];

        const submitted = gpu.endFrame();
        const validationError = await device.popErrorScope();
        await submitted;

        if(validationError)
        {
            fail(`step ${step}: WebGPU validation error: ${validationError.message}`);
        }

        frameRecords.push({
            step,
            probes: gpuContext.frameProbes.map((probe) => ({
                shader: probe.shader,
                runIndex: probe.runIndex,
                probeWord: probe.probeWord,
            })),
            dispatchLog: gpuContext.dispatchLog.map((entry) =>
                `${entry.shader}:${entry.dispatch.mode === "indirect" ? "indirect" : entry.dispatch.counts.join("x")}`),
        });
    }

    // ------------------------------------------------------------------
    // readback (all steps at once; every frame's copies are already submitted)
    // ------------------------------------------------------------------

    await device.queue.onSubmittedWorkDone();

    const particleWords = await mapArray(
        staging.particles, Float32Array, stepCount * particleBytes);
    const gridWords = await mapArray(
        staging.grid, Int32Array, stepCount * gridStepBytes);
    const bukkitWords = await mapArray(
        staging.bukkit, Uint32Array, stepCount * 16);
    const probeWords = await mapArray(
        staging.probes, Int32Array, stepCount * probesPerFrame * 16);

    const counters = await readCounters(device, gpuContext);

    // ------------------------------------------------------------------
    // assemble per-step records
    // ------------------------------------------------------------------

    const particleFloatStride = gpu.ParticleFloatCount;
    const steps = [];

    for(let step = 0; step < stepCount; ++step)
    {
        const particleBase = step * particleCount * particleFloatStride;
        const words = Array.from(particleWords.subarray(
            particleBase, particleBase + particleCount * particleFloatStride));

        const particles = [];
        for(let i = 0; i < particleCount; ++i)
        {
            const offset = i * particleFloatStride;
            particles.push({
                position: [words[offset + 0], words[offset + 1]],
                displacement: [words[offset + 2], words[offset + 3]],
                deformationGradient: [words[offset + 4], words[offset + 5], words[offset + 6], words[offset + 7]],
                deformationDisplacement: [words[offset + 8], words[offset + 9], words[offset + 10], words[offset + 11]],
                liquidDensity: words[offset + 12],
                mass: words[offset + 13],
                material: words[offset + 14],
                volume: words[offset + 15],
                lambda: words[offset + 16],
                logJp: words[offset + 17],
                color: [words[offset + 20], words[offset + 21], words[offset + 22]],
                enabled: words[offset + 23],
            });
        }

        const gridBase = step * 3 * GRID_SIZE[0] * GRID_SIZE[1] * 4;
        const grid = [0, 1, 2].map((index) => ({
            label: `gridBuffer${index}`,
            words: Array.from(gridWords.subarray(
                gridBase + index * GRID_SIZE[0] * GRID_SIZE[1] * 4,
                gridBase + (index + 1) * GRID_SIZE[0] * GRID_SIZE[1] * 4)),
        }));

        const probeRecords = frameRecords[step].probes.map((probe) =>
        {
            const workgroups = probe.probeWord < 0
                ? null
                : Array.from(probeWords.subarray(probe.probeWord * 4, probe.probeWord * 4 + 4));
            return {shader: probe.shader, runIndex: probe.runIndex, workgroups};
        });

        steps.push({
            index: step,
            timeSeconds: (step + 1) * (1.0 / inputs.simRate),
            particleCount,
            particleWords: words,
            particles,
            grid,
            probes: probeRecords,
            bukkitDispatchArgs: Array.from(bukkitWords.subarray(step * 4, step * 4 + 4)),
            dispatchLog: frameRecords[step].dispatchLog,
            derived: deriveStep(particles),
        });
    }

    const elapsedMs = performance.now() - startedAt;

    const trace = {
        meta: {
            schema: "pbmpm-harness/stage2-case/v1",
            upstream: {
                repository: "Electronic Arts pbmpm",
                path: upstreamProvenancePath(),
                commit: upstreamCommit(),
                commitSubject: upstreamCommitSubject(),
            },
            shaderSha256: shaderHashes(),
            runtime: runtime.describe(),
            case: {
                id: `${family.id}-k${iterationCount}-s${stepCount}`,
                family: family.id,
                material: family.material,
                materialName: family.enumName,
                iterationCount,
                steps: stepCount,
                updateCallsPerStep: 1,
                substepsPerUpdateCall: 2,
                simulatedSubstepsPerUpdateCall: 1,
                message: "each step entry is one simulated substep; the bootstrap substep of each update() call (zero g2p2g workgroups) is not reported",
            },
            inputs: describeInputs(inputs, family, stepCount),
            provenance: provenance(),
            particleLayout: {
                source: "shaders/particle.inc.wgsl struct Particle",
                floatStride: gpu.ParticleFloatCount,
                byteStride: gpu.ParticleByteStride,
                fields: [
                    {name: "position", type: "vec2f", floatOffset: 0},
                    {name: "displacement", type: "vec2f", floatOffset: 2},
                    {name: "deformationGradient", type: "mat2x2f", floatOffset: 4, layout: "column-major"},
                    {name: "deformationDisplacement", type: "mat2x2f", floatOffset: 8, layout: "column-major"},
                    {name: "liquidDensity", type: "f32", floatOffset: 12},
                    {name: "mass", type: "f32", floatOffset: 13},
                    {name: "material", type: "f32", floatOffset: 14},
                    {name: "volume", type: "f32", floatOffset: 15},
                    {name: "lambda", type: "f32", floatOffset: 16},
                    {name: "logJp", type: "f32", floatOffset: 17},
                    {name: "color", type: "vec3f", floatOffset: 20, note: "floats 18-19 are std430 alignment padding"},
                    {name: "enabled", type: "f32", floatOffset: 23},
                ],
            },
            seed: {
                blockOrigin: BLOCK_ORIGIN,
                blockSize: BLOCK_SIZE,
                particleCount,
                velocityField: {
                    expansionRate: EXPANSION_RATE,
                    shearRate: SHEAR_RATE,
                    center: BLOCK_CENTER,
                },
                familyState: familyStateDescription(family),
                rows: seed.rows.map((row) => particleRowToWords(row)),
            },
            counters,
        },
        steps,
    };

    trace.meta.verification = verifyCase(trace, seed);

    return {trace, elapsedMs};
}

function particleRowToWords(row)
{
    // Float32 rounding mirrors what writeBuffer does, so these words are
    // bit-identical to what the GPU seed contains.
    const f = Math.fround;
    const words = new Array(gpu.ParticleFloatCount).fill(0);
    words[0] = f(row.position[0]);
    words[1] = f(row.position[1]);
    words[2] = f(row.displacement[0]);
    words[3] = f(row.displacement[1]);
    words[4] = f(row.deformationGradient[0]);
    words[5] = f(row.deformationGradient[1]);
    words[6] = f(row.deformationGradient[2]);
    words[7] = f(row.deformationGradient[3]);
    words[8] = f(row.deformationDisplacement[0]);
    words[9] = f(row.deformationDisplacement[1]);
    words[10] = f(row.deformationDisplacement[2]);
    words[11] = f(row.deformationDisplacement[3]);
    words[12] = f(row.liquidDensity);
    words[13] = f(row.mass);
    words[14] = f(row.material);
    words[15] = f(row.volume);
    words[16] = f(row.lambda);
    words[17] = f(row.logJp);
    words[20] = f(row.color[0]);
    words[21] = f(row.color[1]);
    words[22] = f(row.color[2]);
    words[23] = f(row.enabled);
    return words;
}

function familyStateDescription(family)
{
    switch(family.id)
    {
        case "mixed":
            return "36 rows: liquid 4x4 centres (8.5..11.5,8.5..11.5); sand 4x4 (12.5..15.5,8.5..11.5); elastic 2x2 (9.5..10.5,12.5..13.5). One-cell spacing < quadratic support diameter 3: shared nonzero stencil nodes at both interfaces. F = I, logJp = 0; same velocity field and profile as homogeneous cases.";
        case "liquid":
            return "material 0; F = I, logJp = 0. Liquid never writes deformationGradient; it carries volume in liquidDensity and applies deviatoric damping + hydrostatic relaxation to deformationDisplacement.";
        case "elastic":
            return "material 1; F = I, logJp = 0. Exercises the `MaterialElastic || MaterialVisco` MPM integration branch: F = (I + dF)*F, SVD safety clamp, and the elasticityRatio=1 relaxation target U*Vt (closest rotation).";
        case "sand":
            return "material 2; F = I, logJp = 0 (no carried dilation), frictionAngle 30. Exercises the Drucker-Prager branch: Sigma clamp when logJp == 0, trace = tr(log Sigma) + logJp, and logJp = 0.5*trace on expansion / cone projection on compression.";
        case "visco":
            return "material 3; F = I, logJp = 0, plasticity 0.9. Exercises the visco-plastic branch: Sigma clamped to [1/yieldSurface, yieldSurface] with yieldSurface = exp(1 - plasticity), then rescaled to preserve J.";
        default:
            return "";
    }
}

// ------------------------------------------------------------------
// per-step derived summary (no post-processing of stored values)
// ------------------------------------------------------------------

function deriveStep(particles)
{
    let maxSigma = 0;
    let minSigma = Infinity;
    let maxRatio = 0;
    let minDet = Infinity;
    let maxDet = -Infinity;
    let minLogJp = Infinity;
    let maxLogJp = -Infinity;
    let maxAbsFDeviation = 0;
    let meanDisplacementY = 0;
    let meanPositionY = 0;

    for(const particle of particles)
    {
        const [sigmaMax, sigmaMin] = singularValues(particle.deformationGradient);
        maxSigma = Math.max(maxSigma, sigmaMax);
        minSigma = Math.min(minSigma, sigmaMin);
        if(sigmaMin > 0)
        {
            maxRatio = Math.max(maxRatio, sigmaMax / sigmaMin);
        }

        const detF = determinant(particle.deformationGradient);
        minDet = Math.min(minDet, detF);
        maxDet = Math.max(maxDet, detF);

        minLogJp = Math.min(minLogJp, particle.logJp);
        maxLogJp = Math.max(maxLogJp, particle.logJp);

        maxAbsFDeviation = Math.max(maxAbsFDeviation,
            Math.abs(particle.deformationGradient[0] - 1),
            Math.abs(particle.deformationGradient[1]),
            Math.abs(particle.deformationGradient[2]),
            Math.abs(particle.deformationGradient[3] - 1));

        meanDisplacementY += particle.displacement[1] / particles.length;
        meanPositionY += particle.position[1] / particles.length;
    }

    return {
        maxSigma,
        minSigma,
        maxRatio,
        minDet,
        maxDet,
        minLogJp,
        maxLogJp,
        maxAbsFDeviationFromIdentity: maxAbsFDeviation,
        meanDisplacementY,
        meanPositionY,
    };
}

// Mirrors matrix.inc.wgsl det(); column-major storage as in shaders.
function determinant(m)
{
    return m[0] * m[3] - m[2] * m[1];
}

// Singular values of a 2x2 matrix from its invariants:
// sigma1^2 + sigma2^2 = frobenius^2, sigma1 * sigma2 = |det|.
function singularValues(m)
{
    const frobeniusSquared =
        m[0] * m[0] + m[1] * m[1] + m[2] * m[2] + m[3] * m[3];
    const det = determinant(m);
    const discriminant = Math.max(0, frobeniusSquared * frobeniusSquared - 4 * det * det);

    const sigmaMax = Math.sqrt((frobeniusSquared + Math.sqrt(discriminant)) / 2);
    const sigmaMin = sigmaMax === 0 ? 0 : Math.abs(det) / sigmaMax;

    return [sigmaMax, sigmaMin];
}

// ------------------------------------------------------------------
// verification
// ------------------------------------------------------------------

function verifyCase(trace, seed)
{
    const steps = trace.steps;
    const lastStep = steps[steps.length - 1];
    const family = trace.meta.case.family;
    const inputs = trace.meta.inputs;
    const yieldSurface = Math.exp(1 - inputs.plasticity);

    // Frozen fields must be bit-identical to the seed at every step.
    const seedWords = seed.rows.map((row) => particleRowToWords(row));
    const frozenFields = [13, 14, 15, 20, 21, 22, 23]; // mass, material, volume, color, enabled
    let frozenFieldViolations = 0;
    let frozenFieldChecks = 0;

    for(const step of steps)
    {
        for(let i = 0; i < step.particleCount; ++i)
        {
            const offset = i * gpu.ParticleFloatCount;
            for(const field of frozenFields)
            {
                frozenFieldChecks += 1;
                if(!Object.is(step.particleWords[offset + field], seedWords[i][field]))
                {
                    frozenFieldViolations += 1;
                }
            }
        }
    }

    // Non-finite scan over every stored float of every step.
    const nonFinite = [];
    for(const step of steps)
    {
        for(let i = 0; i < step.particleWords.length; ++i)
        {
            if(!Number.isFinite(step.particleWords[i]))
            {
                nonFinite.push({step: step.index, word: i, value: String(step.particleWords[i])});
            }
        }
    }

    // Work proof: every reported step must show 2*K g2p2g probes, K of them with
    // zero workgroups (the un-reported bootstrap substep) and K with real work.
    const workProof = steps.map((step) =>
    {
        const run = step.probes.filter((probe) => probe.shader === "g2p2g");
        const workgroups = run.map((probe) => probe.workgroups ? probe.workgroups[0] : null);
        const bootstrap = workgroups.slice(0, inputs.iterationCount);
        const simulated = workgroups.slice(inputs.iterationCount);

        return {
            step: step.index,
            g2p2gWorkgroups: workgroups,
            bootstrapAllZero: bootstrap.every((count) => count === 0),
            simulatedAllNonZero: simulated.length === inputs.iterationCount
                && simulated.every((count) => count > 0),
        };
    });

    const allStepsDidWork = workProof.every((entry) =>
        entry.bootstrapAllZero && entry.simulatedAllNonZero);

    // Family signature observables.
    const signature = familySignature(family, steps, yieldSurface, inputs);

    return {
        finiteValues: nonFinite.length === 0,
        nonFiniteCount: nonFinite.length,
        nonFiniteValues: nonFinite.slice(0, 32),
        frozenFields: {
            checked: frozenFieldChecks,
            violations: frozenFieldViolations,
            bitExact: frozenFieldViolations === 0,
            fields: ["mass", "material", "volume", "color", "enabled"],
        },
        conservation: {
            particleCountStart: seed.rows.length,
            particleCountEnd: trace.meta.counters.particleCount,
            freeCountEnd: trace.meta.counters.freeCount,
            massSum: steps.map((step) => step.particles.reduce((sum, p) => sum + p.mass, 0)),
            volumeSum: steps.map((step) => step.particles.reduce((sum, p) => sum + p.volume, 0)),
        },
        workProof,
        allStepsDidWork,
        signature,
    };
}

function familySignature(family, steps, yieldSurface, inputs)
{
    if(family === "mixed")
    {
        const signatures = ["liquid", "elastic", "sand"].map((id) =>
        {
            const material = FAMILIES.find((entry) => entry.id === id).material;
            const subset = steps.map((step) =>
            {
                const particles = step.particles.filter((p) => p.material === material);
                return {...step, particles, derived: deriveStep(particles)};
            });
            return familySignature(id, subset, yieldSurface, inputs);
        });
        return {branch: "mixed", signatures, passed: signatures.every((entry) => entry.passed)};
    }

    const last = steps[steps.length - 1];

    const sigmaStats = (step) =>
    {
        let sigmaMax = 0;
        let ratio = 0;
        let detMin = Infinity;
        let detMax = -Infinity;
        for(const particle of step.particles)
        {
            const [s1, s2] = singularValues(particle.deformationGradient);
            sigmaMax = Math.max(sigmaMax, s1);
            if(s2 > 0)
            {
                ratio = Math.max(ratio, s1 / s2);
            }
            const det = determinant(particle.deformationGradient);
            detMin = Math.min(detMin, det);
            detMax = Math.max(detMax, det);
        }
        return {sigmaMax, ratio, detMin, detMax};
    };

    if(family === "liquid")
    {
        let identity = true;
        let logJpZero = true;
        for(const step of steps)
        {
            for(const particle of step.particles)
            {
                const f = particle.deformationGradient;
                if(f[0] !== 1 || f[1] !== 0 || f[2] !== 0 || f[3] !== 1)
                {
                    identity = false;
                }
                if(particle.logJp !== 0)
                {
                    logJpZero = false;
                }
            }
        }

        // The liquid branch carries volume in liquidDensity instead of F, so a
        // liquidDensity that leaves 1 proves the liquid volume/relaxation path
        // ran while F stayed untouched.
        let densityMoved = false;
        for(const step of steps)
        {
            for(const particle of step.particles)
            {
                if(particle.liquidDensity !== 1)
                {
                    densityMoved = true;
                }
            }
        }

        const stats = sigmaStats(last);
        return {
            branch: "liquid",
            proves: "g2p2g's MaterialLiquid path leaves deformationGradient untouched, so a bit-exact identity F at every step proves the liquid branch ran and the solid integration/plasticity path did not; liquidDensity != 1 proves the liquid volume-constraint/relaxation code also ran",
            deformationGradientIsIdentityBitExact: identity,
            logJpZeroBitExact: logJpZero,
            liquidDensityMovedFromOne: densityMoved,
            sigmaMaxAtEnd: stats.sigmaMax,
            detRangeAtEnd: [stats.detMin, stats.detMax],
            passed: identity && logJpZero && densityMoved,
        };
    }

    if(family === "elastic")
    {
        // Elastic (elasticityRatio = 1) integrates F = (I+dF)*F with only the
        // [0.1, 10000] safety clamp and no plasticity, so the stored F carries
        // the accumulated stretch: sigma != 1 and det != 1, unlike Liquid
        // (F = I), Sand (Sigma collapsed / cone-projected into logJp) and Visco
        // (anisotropy clamped to the yield surface).
        const stats = sigmaStats(last);
        const integrated = last.derived.maxAbsFDeviationFromIdentity > 0.005;
        const detDeviates = Math.max(Math.abs(stats.detMin - 1), Math.abs(stats.detMax - 1)) > 0.02;
        const noPlasticity = steps.every((step) => step.particles.every((p) => p.logJp === 0));

        return {
            branch: "elastic",
            proves: "F != I while logJp stays bit-exact 0 and det(F) deviates from 1: the `MaterialElastic || MaterialVisco` MPM integration F = (I+dF)*F with the elasticityRatio=1 relaxation target U*Vt ran and carried stretch without any plasticity term",
            maxAbsFDeviationAtEnd: last.derived.maxAbsFDeviationFromIdentity,
            sigmaMaxAtEnd: stats.sigmaMax,
            detRangeAtEnd: [stats.detMin, stats.detMax],
            ratioAtEnd: stats.ratio,
            viscoRatioBound: yieldSurface * yieldSurface,
            logJpZeroBitExact: noPlasticity,
            passed: integrated && detDeviates && noPlasticity,
        };
    }

    if(family === "sand")
    {
        // Sand's Drucker-Prager branch is the only place logJp is ever written.
        // Seed logJp = 0 bit-exact, so logJp != 0 can only come from that branch:
        // on expansion it stores beta*trace and resets Sigma to exactly 1, on
        // compression it projects Sigma onto the cone (det(F) collapses far below
        // the elastic value) while resetting logJp to 0.
        const logJpSeries = steps.map((step) => ({
            step: step.index,
            min: step.derived.minLogJp,
            max: step.derived.maxLogJp,
        }));
        const sigmaMaxSeries = steps.map((step) => step.derived.maxSigma);
        const detMinSeries = steps.map((step) => step.derived.minDet);

        const logJpActiveEveryStep = logJpSeries.every((entry) => entry.max > 0);
        const finalLogJp = last.derived.maxLogJp;
        const deformationWritten = last.derived.maxAbsFDeviationFromIdentity > 0.005;
        const volumeCollapse = Math.min(...detMinSeries) < 0.9;

        return {
            branch: "sand",
            proves: "logJp is bit-exact 0 in the seed yet non-zero at every step, and particle.logJp is written only inside the MaterialSand Drucker-Prager branch (0.5*trace on expansion, cone projection on compression); the same branch leaves Sigma <= 1 and lets det(F) collapse far below the elastic case, proving the plasticity projection ran too",
            logJpAtEnd: finalLogJp,
            logJpSeries,
            sigmaMaxSeries,
            detMinSeries,
            logJpActiveEveryStep,
            deformationGradientWritten: deformationWritten,
            volumeCollapseBelowElasticRange: volumeCollapse,
            passed: logJpActiveEveryStep && deformationWritten && volumeCollapse,
        };
    }

    // visco
    const ratioBound = yieldSurface * yieldSurface;
    const tolerance = 1e-4;
    const ratioSeries = steps.map((step) => ({step: step.index, ratio: step.derived.maxRatio}));
    const boundedEverywhere = ratioSeries.every((entry) => entry.ratio <= ratioBound * (1 + tolerance));
    const clampActiveSteps = ratioSeries
        .filter((entry) => entry.ratio >= ratioBound * (1 - 1e-3))
        .map((entry) => entry.step);
    const clampActive = clampActiveSteps.length > 0;
    const detPreserved = Math.max(
        Math.abs(last.derived.minDet - 1),
        Math.abs(last.derived.maxDet - 1)) > 0.02;

    return {
        branch: "visco",
        proves: "max sigma ratio (sigma1/sigma2) is pinned to yieldSurface^2 = exp(2*(1-plasticity)) = 1.22140276 on the steps where the material yields, and never exceeds it: the MaterialVisco branch clamps Sigma to [1/yieldSurface, yieldSurface] and rescales to preserve J, so anisotropy is capped while det(F) keeps evolving. Elastic under the identical seed exceeds Visco's ratio at the same K",
        yieldSurface,
        ratioBound,
        ratioSeries,
        boundedEverywhere,
        clampActiveSteps,
        clampActive,
        detPreserved,
        passed: boundedEverywhere && clampActive,
    };
}

// ------------------------------------------------------------------
// readback helpers
// ------------------------------------------------------------------

async function mapArray(buffer, Type, byteLength)
{
    await buffer.mapAsync(GPUMapMode.READ, 0, byteLength);
    const view = new Type(buffer.getMappedRange(0, byteLength)).slice();
    buffer.unmap();
    return view;
}

async function readCounters(device, gpuContext)
{
    const stagingBuffer = device.createBuffer({
        label: "stage2CounterStaging",
        size: 8,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    const encoder = device.createCommandEncoder({label: "stage2Counters"});
    encoder.copyBufferToBuffer(gpuContext.particleCountBuffer, 0, stagingBuffer, 0, 4);
    encoder.copyBufferToBuffer(gpuContext.particleFreeIndicesBuffer, 0, stagingBuffer, 4, 4);
    device.queue.submit([encoder.finish()]);

    await device.queue.onSubmittedWorkDone();
    const values = await mapArray(stagingBuffer, Int32Array, 8);

    return {particleCount: values[0], freeCount: values[1]};
}

// ------------------------------------------------------------------
// cross-family checks (same seed, same step count, only the material differs)
// ------------------------------------------------------------------

function computeCrossFamilyChecks(indexCases)
{
    const byIteration = new Map();

    for(const entry of indexCases)
    {
        if(entry.family === "mixed")
            continue;

        if(!byIteration.has(entry.iterationCount))
        {
            byIteration.set(entry.iterationCount, {});
        }
        byIteration.get(entry.iterationCount)[entry.family] = entry;
    }

    const checks = [];

    for(const iterationCount of [...byIteration.keys()].sort((a, b) => a - b))
    {
        const group = byIteration.get(iterationCount);
        if(!group.liquid || !group.elastic || !group.sand || !group.visco)
        {
            checks.push({iterationCount, passed: false, reason: "missing family in group"});
            continue;
        }

        const yieldSurface = Math.exp(1 - 0.9);
        const ratioBound = yieldSurface * yieldSurface;

        const checksByPoint = {
            liquidDeformationGradientIsIdentity: group.liquid.summary.fIdentityBitExact,
            solidsDeformDeformationGradient:
                group.elastic.summary.maxAbsFDeviationFromIdentity > 0.005
                && group.sand.summary.maxAbsFDeviationFromIdentity > 0.005
                && group.visco.summary.maxAbsFDeviationFromIdentity > 0.005,
            sandWritesLogJp: group.sand.summary.logJpRange[1] > 0,
            nonSandLogJpStaysZero:
                group.liquid.summary.logJpRange[0] === 0 && group.liquid.summary.logJpRange[1] === 0
                && group.elastic.summary.logJpRange[0] === 0 && group.elastic.summary.logJpRange[1] === 0
                && group.visco.summary.logJpRange[0] === 0 && group.visco.summary.logJpRange[1] === 0,
            viscoAnisotropyBounded: group.visco.summary.maxRatio <= ratioBound * (1 + 1e-4),
            viscoClampActive: group.visco.summary.clampActiveSteps.length > 0,
            elasticExceedsViscoAnisotropy: group.elastic.summary.maxRatio > group.visco.summary.maxRatio,
            sandVolumeCollapseBelowElastic:
                group.sand.summary.detRange[0] < group.elastic.summary.detRange[0],
        };

        const passed = Object.values(checksByPoint).every((value) => value === true);

        checks.push({
            iterationCount,
            steps: group.liquid.steps,
            yieldSurface,
            ratioBound,
            ratios: {
                elastic: group.elastic.summary.maxRatio,
                visco: group.visco.summary.maxRatio,
            },
            logJp: {
                sand: group.sand.summary.logJpRange[1],
                elastic: group.elastic.summary.logJpRange[1],
                liquid: group.liquid.summary.logJpRange[1],
                visco: group.visco.summary.logJpRange[1],
            },
            checks: checksByPoint,
            passed,
        });
    }

    return checks;
}

// ------------------------------------------------------------------
// upstream metadata
// ------------------------------------------------------------------

function upstreamCommit()
{
    // See run.mjs: the vendored tree carries no git history of its own.
    if(UPSTREAM_DIR === VENDORED_UPSTREAM_DIR)
    {
        return UPSTREAM_COMMIT;
    }

    try
    {
        return execFileSync("git", ["-C", UPSTREAM_DIR, "rev-parse", "HEAD"], {encoding: "utf8"}).trim();
    }
    catch
    {
        return UPSTREAM_COMMIT;
    }
}

function upstreamCommitSubject()
{
    if(UPSTREAM_DIR === VENDORED_UPSTREAM_DIR)
    {
        return UPSTREAM_COMMIT_SUBJECT;
    }

    try
    {
        return execFileSync("git", ["-C", UPSTREAM_DIR, "log", "-1", "--format=%s"], {encoding: "utf8"}).trim();
    }
    catch
    {
        return null;
    }
}

function upstreamProvenancePath()
{
    return UPSTREAM_DIR === VENDORED_UPSTREAM_DIR ? "scripts/pbmpm/upstream" : UPSTREAM_DIR;
}

function shaderHashes()
{
    const names = readdirSync(join(UPSTREAM_DIR, "shaders"))
        .filter((name) => name.endsWith(".wgsl")).sort();

    const hashes = {};
    for(const name of names)
    {
        const bytes = readFileSync(join(UPSTREAM_DIR, "shaders", name));
        hashes[name] = createHash("sha256").update(bytes).digest("hex");
    }

    return hashes;
}

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------

async function main()
{
    const instance = create([]);
    const adapter = await instance.requestAdapter();
    if(!adapter)
    {
        fail("no WebGPU adapter available");
    }

    const adapterInfo = adapter.info ?? {};
    const device = await adapter.requestDevice();

    const deviceErrors = [];
    device.addEventListener("uncapturederror", (event) =>
    {
        deviceErrors.push(event.error?.message ?? String(event.error));
    });
    device.lost.then((info) => fail(`device lost: ${info.reason} ${info.message}`));

    const insertHandlers = {};
    sim.init(insertHandlers);

    // shader.init() also compiles the particle render module, which needs the
    // RenderConstants struct text that upstream render.js generates. This is the
    // identical text produced by BufferFactory('RenderConstants', Uniform).
    insertHandlers["RenderConstants"] = [
        "struct RenderConstants",
        "{",
        "particleRadiusTimestamp: vec2f,",
        "canvasSize: vec2f,",
        "viewPos: vec2f,",
        "viewExtent: vec2f,",
        "renderMode: f32,",
        "deltaTime: f32,",
        "};",
        "",
    ].join("\n");

    await gpu.init(device, insertHandlers);

    const gpuContext = gpu.getGpuContext();
    gpuContext.device = device;

    const runtime = {
        device,
        gpuContext,
        describe: () => ({
            node: process.version,
            webgpuModule: "webgpu@0.6.2 (Dawn node bindings)",
            adapterVendor: adapterInfo.vendor ?? null,
            adapterArchitecture: adapterInfo.architecture ?? null,
            adapterDevice: adapterInfo.device ?? null,
            adapterDescription: adapterInfo.description ?? null,
        }),
    };

    mkdirSync(OUT_DIR, {recursive: true});

    const indexCases = [];
    const timings = [];
    const failures = [];

    for(const familyId of FAMILY_FILTER)
    {
        const family = FAMILIES.find((entry) => entry.id === familyId);
        if(!family)
        {
            fail(`unknown family [${familyId}]`);
        }

        for(const iterationCount of ITERATION_FILTER)
        {
            const {trace, elapsedMs} = await runCase(runtime, family, iterationCount, STEP_COUNT);

            const id = trace.meta.case.id;
            const fileName = `${id}.json`;
            const filePath = join(OUT_DIR, fileName);
            const json = JSON.stringify(trace);
            writeFileSync(filePath, json);

            const sha256 = createHash("sha256").update(json).digest("hex");
            const verification = trace.meta.verification;

            if(deviceErrors.length)
            {
                failures.push({id, reason: `device errors: ${deviceErrors.join(" | ")}`});
            }
            if(!verification.finiteValues)
            {
                failures.push({id, reason: `non-finite values: ${JSON.stringify(verification.nonFiniteValues)}`});
            }
            if(!verification.frozenFields.bitExact)
            {
                failures.push({id, reason: `frozen field violations: ${verification.frozenFields.violations}`});
            }
            if(!verification.allStepsDidWork)
            {
                failures.push({id, reason: `a reported step did not do g2p2g work: ${JSON.stringify(verification.workProof)}`});
            }
            if(!verification.signature.passed)
            {
                failures.push({id, reason: `family signature check failed: ${JSON.stringify(verification.signature)}`});
            }

            indexCases.push({
                id,
                family: familyId,
                material: family.material,
                iterationCount,
                steps: STEP_COUNT,
                particleCount: trace.meta.seed.particleCount,
                gridSize: GRID_SIZE,
                simulatedTimeSeconds: STEP_COUNT * (1.0 / trace.meta.inputs.simRate),
                file: fileName,
                sha256,
                summary: {
                    maxSigma: trace.steps[STEP_COUNT - 1].derived.maxSigma,
                    maxRatio: trace.steps[STEP_COUNT - 1].derived.maxRatio,
                    detRange: [trace.steps[STEP_COUNT - 1].derived.minDet, trace.steps[STEP_COUNT - 1].derived.maxDet],
                    logJpRange: [trace.steps[STEP_COUNT - 1].derived.minLogJp, trace.steps[STEP_COUNT - 1].derived.maxLogJp],
                    maxAbsFDeviationFromIdentity: trace.steps[STEP_COUNT - 1].derived.maxAbsFDeviationFromIdentity,
                    fIdentityBitExact: verification.signature.deformationGradientIsIdentityBitExact ?? false,
                    clampActiveSteps: verification.signature.clampActiveSteps ?? [],
                },
                verification: {
                    finiteValues: verification.finiteValues,
                    frozenFieldsBitExact: verification.frozenFields.bitExact,
                    allStepsDidWork: verification.allStepsDidWork,
                    signaturePassed: verification.signature.passed,
                },
            });

            timings.push({id, wallTimeMs: Math.round(elapsedMs)});

            const status = indexCases[indexCases.length - 1].verification;
            console.log(
                `${id.padEnd(16)} ${String(Math.round(elapsedMs)).padStart(6)} ms  `
                + `finite=${status.finiteValues} frozen=${status.frozenFieldsBitExact} `
                + `work=${status.allStepsDidWork} signature=${status.signaturePassed} `
                + `sigma=${indexCases[indexCases.length - 1].summary.maxSigma.toFixed(4)} `
                + `ratio=${indexCases[indexCases.length - 1].summary.maxRatio.toFixed(4)} `
                + `logJp=${indexCases[indexCases.length - 1].summary.logJpRange[1].toFixed(6)}`);
        }
    }

    const crossFamilyChecks = computeCrossFamilyChecks(indexCases);

    for(const check of crossFamilyChecks)
    {
        if(!check.passed)
        {
            failures.push({id: `cross-family-k${check.iterationCount}`, reason: `cross-family check failed: ${JSON.stringify(check)}`});
        }
    }

    const index = {
        schema: "pbmpm-harness/stage2-index/v1",
        upstream: {
            repository: "Electronic Arts pbmpm",
            path: upstreamProvenancePath(),
            commit: upstreamCommit(),
            commitSubject: upstreamCommitSubject(),
        },
        shaderSha256: shaderHashes(),
        runtime: runtime.describe(),
        gridSize: GRID_SIZE,
        resolution: RESOLUTION,
        steps: STEP_COUNT,
        families: FAMILY_FILTER,
        iterationCounts: ITERATION_FILTER,
        seed: {
            blockOrigin: BLOCK_ORIGIN,
            blockSize: BLOCK_SIZE,
            particlesPerCase: FAMILY_FILTER.includes("mixed") ? null : BLOCK_SIZE[0] * BLOCK_SIZE[1],
            velocityField: {
                expansionRate: EXPANSION_RATE,
                shearRate: SHEAR_RATE,
                center: BLOCK_CENTER,
            },
        },
        cases: indexCases,
        crossFamilyChecks,
        failures,
    };

    writeFileSync(join(OUT_DIR, "index.json"), JSON.stringify(index, null, 2));
    writeFileSync(join(OUT_DIR, "timings.json"), JSON.stringify({
        note: "wall time is environment dependent and deliberately kept out of index.json so the case files and index stay byte-comparable across runs",
        cases: timings,
    }, null, 2));

    console.log("");
    console.log(`cases written : ${indexCases.length} -> ${relative(process.cwd(), OUT_DIR) || "."}`);
    console.log(`index         : ${relative(process.cwd(), join(OUT_DIR, "index.json"))}`);
    console.log(`failures      : ${failures.length}`);

    if(failures.length)
    {
        console.error(JSON.stringify(failures, null, 2));
        process.exit(1);
    }
}

try
{
    await main();
}
catch(error)
{
    fail(error?.stack ?? String(error));
}
