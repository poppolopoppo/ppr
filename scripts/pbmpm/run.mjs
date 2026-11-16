//-----------------------------------------------------------------------------
// Headless oracle harness for the EA PB-MPM WebGPU demo (stage 1).
//
// Runs the *upstream* simulation pipeline (src/sim.js + shaders/*.wgsl, copied
// verbatim) on a headless Dawn device, without DOM, canvas or network.
//
//   node run.mjs --out out/liquid-k2-s1.json        # the stage-1 trace
//   node run.mjs --out out/diagnostic-4.json --steps 4   # diagnostic: 4 frames
//
// Only src/gpu.js in this tool is a replacement for upstream code; it provides
// the compute-only subset of gpu.js that sim.js consumes. Everything it imports
// from upstream is vendored under upstream/ (see README.md for the pin).
//-----------------------------------------------------------------------------

"use strict";

import {createRequire} from "node:module";
import {readdirSync, readFileSync, writeFileSync, mkdirSync} from "node:fs";
import {createHash} from "node:crypto";
import {execFileSync} from "node:child_process";
import {dirname, join} from "node:path";
import {fileURLToPath} from "node:url";

// ------------------------------------------------------------------
// configuration
// ------------------------------------------------------------------

const HARNESS_DIR = dirname(fileURLToPath(import.meta.url));

// Vendored upstream checkout: upstream/src/*.js and upstream/shaders/*.wgsl are
// verbatim copies of the pinned commit (upstream/README.md records the pin and
// the hashes). PBMPM_UPSTREAM overrides it with another checkout.
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

const OUT_PATH = argValue("--out", join(HARNESS_DIR, "out", "liquid-k2-s1.json"));
const FRAME_COUNT = Number(argValue("--steps", "1"));

// Upstream renders on requestAnimationFrame; the harness steps time explicitly.
const FRAME_DELTA_MS = 1000.0 / 60.0;

// Simulation domain. Upstream derives gridSize as floor(canvas / simResDivisor);
// the harness picks a small fixed grid instead, with resolution = gridSize * divisor
// so that mouse/screen <-> grid coordinate mapping stays coherent.
const GRID_SIZE = [32, 32];
const SIM_RES_DIVISOR = 8;
const RESOLUTION = [GRID_SIZE[0] * SIM_RES_DIVISOR, GRID_SIZE[1] * SIM_RES_DIVISOR];

// Seed block: 8x8 cells, one particle per cell at the cell centre, well inside
// the guardian border (particle.inc.wgsl requires 4 <= id <= gridSize-4).
const BLOCK_ORIGIN = [8, 8];
const BLOCK_SIZE = [8, 8];

// ------------------------------------------------------------------
// webgpu runtime
// ------------------------------------------------------------------

const require = createRequire(import.meta.url);
const {create, globals} = require("webgpu");

// Browser-style globals: GPUBufferUsage, GPUMapMode, GPUBuffer, GPUDevice, ...
for(const key of Object.keys(globals))
{
    globalThis[key] = globals[key];
}

// Harness-side instrumentation. It only (a) adds COPY_SRC to storage buffers so
// the trace can read back buffers that sim.js creates as STORAGE|COPY_DST (the
// grid and bukkit buffers), and (b) records created buffers by label. It does
// not change contents or usage semantics.
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

// ------------------------------------------------------------------
// shader fetch shim (upstream shader.js loads shaders/<name>.wgsl)
// ------------------------------------------------------------------

globalThis.fetch = async (url) =>
{
    const relativePath = String(url).replace(/^\.\//, "");
    const shaderText = readFileSync(join(UPSTREAM_DIR, relativePath), "utf8");
    return {
        ok: true,
        status: 200,
        async text() {return shaderText;}
    };
};

// ------------------------------------------------------------------
// modules
// ------------------------------------------------------------------

const sim = await import("./upstream/src/sim.js");
const gpu = await import("./src/gpu.js");

function fail(message)
{
    console.error(`FATAL: ${message}`);
    process.exit(1);
}

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

    // ------------------------------------------------------------------
    // upstream initialization (identical call order to main.js, minus ui/render)
    // ------------------------------------------------------------------

    const insertHandlers = {};
    sim.init(insertHandlers);

    // shader.init() compiles every entry of shader.Shaders, including the
    // particle *render* module, which needs the RenderConstants struct text and
    // RenderEnums. Upstream supplies RenderConstants from src/render.js, which
    // the harness deliberately does not copy. This is the identical text that
    // buffer_factory.BufferFactory('RenderConstants', Uniform) produces for
    // render.js's field list (six members, 16 words, no generated padding).
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

    // ------------------------------------------------------------------
    // inputs
    // ------------------------------------------------------------------

    const inputs = buildInputs();
    const config = inputsConfig(inputs);

    // ------------------------------------------------------------------
    // reset + deterministic particle seed
    // ------------------------------------------------------------------

    gpu.resetBuffers(inputs.gridSize);

    const seed = buildSeedParticles();
    gpu.seedParticles(seed.particles, seed.particles.length);

    // ------------------------------------------------------------------
    // upstream update() calls
    //
    // time.doTimeRegulation yields floor(frameDelta * simRate / 1000) substeps.
    // With a 60 Hz frame delta and simRate 120 that is exactly 2 substeps:
    //   substep 0 - the per-update() bukkit dispatch buffer is still the blank
    //               [0,1,1,0], so every g2p2g dispatch this substep is empty. It
    //               only runs emission (a no-op here) and bukkitizes the seeded
    //               particles, producing the indirect args for substep 1.
    //   substep 1 - the single simulated substep: g2p2g x iterationCount with
    //               the bukkit dispatch built at the end of substep 0.
    // This bootstrap-then-simulate ordering is upstream's own pipeline shape
    // (sim.update() cannot simulate on substep 0 by construction); the trace is
    // taken after the one simulated substep.
    // ------------------------------------------------------------------

    const frames = [];
    for(let frame = 0; frame < FRAME_COUNT; ++frame)
    {
        // doReset is passed on every frame: in sim.js/time.js it only resets the
        // time accumulator and the substep counter, which is what makes each
        // frame deterministically a 2-substep frame. Simulation state persists
        // because gpu.resetBuffers (main.js's reset path) runs only once above.
        const frameInputs = {
            ...inputs,
            timeStamp: frame * FRAME_DELTA_MS,
            doReset: true,
        };

        gpu.beginFrame();

        device.pushErrorScope("validation");
        sim.update(gpuContext, frameInputs);
        const submitted = gpu.endFrame();
        const validationError = await device.popErrorScope();
        await submitted;

        if(validationError)
        {
            fail(`WebGPU validation error on frame ${frame}: ${validationError.message}`);
        }
        if(deviceErrors.length)
        {
            fail(`WebGPU uncaptured error on frame ${frame}: ${deviceErrors.join(" | ")}`);
        }

        frames.push({
            frame,
            timeStamp: frameInputs.timeStamp,
            doReset: frameInputs.doReset,
            dispatchLog: gpuContext.dispatchLog.slice(),
        });
    }

    // ------------------------------------------------------------------
    // readback
    // ------------------------------------------------------------------

    const gridBuffers = createdBuffers.filter((buffer) => buffer.label?.startsWith("gridBuffer")).slice(-3);
    if(gridBuffers.length !== 3)
    {
        fail(`expected 3 grid buffers, found ${gridBuffers.length}`);
    }

    const bukkitDispatchBuffer = lastBufferLabelled("bukkitParticleDispatchBuffer");
    const bukkitThreadDataBuffer = lastBufferLabelled("bukkitParticleThreadData");

    const readback = await readBackAll(device, gpuContext, {
        particleCount: seed.particles.length,
        gridBuffers,
        bukkitDispatchBuffer,
        bukkitThreadDataBuffer,
    });

    // ------------------------------------------------------------------
    // verification
    // ------------------------------------------------------------------

    const verification = verify(seed.particles, readback, inputs);

    // ------------------------------------------------------------------
    // serialize
    // ------------------------------------------------------------------

    const trace = {
        meta: {
            upstream: {
                repository: "Electronic Arts pbmpm",
                path: upstreamProvenancePath(),
                commit: upstreamCommit(),
                commitSubject: upstreamCommitSubject(),
            },
            shaderSha256: shaderHashes(),
            runtime: {
                node: process.version,
                webgpuModule: "webgpu@0.6.2 (Dawn node bindings)",
                adapterVendor: adapterInfo.vendor ?? null,
                adapterArchitecture: adapterInfo.architecture ?? null,
                adapterDevice: adapterInfo.device ?? null,
                adapterDescription: adapterInfo.description ?? null,
            },
            case: {
                name: FRAME_COUNT === 1 ? "liquid-k2-s1" : `liquid-k2-s1-diagnostic-${FRAME_COUNT}frames`,
                material: "Liquid (SimEnums.MaterialLiquid = 0)",
                frames: FRAME_COUNT,
                substepsPerFrame: 2,
                simulatedSubstepsPerFrame: 1,
                iterationCount: inputs.iterationCount,
                tracedSubstepIndexWithinFrame: 1,
                simulatedTimeSeconds: FRAME_COUNT * (1.0 / inputs.simRate),
                note: "within each update() call substep 0 is upstream's mandatory bukkit bootstrap (empty indirect dispatch); substep 1 is the simulated substep",
            },
            inputs: config.inputs,
            provenance: config.provenance,
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
                gridCells: [BLOCK_ORIGIN, BLOCK_SIZE],
                particlesPerCellAxis: inputs.particlesPerCellAxis,
                particleCount: seed.particles.length,
                description: "one particle per cell at cell centre, 8x8 block at (8,8), all Liquid, F = I, mass = volume = 1",
            },
            frames: frames,
            diagnostics: {
                webgpuValidationErrors: deviceErrors.slice(),
            },
            verification: verification,
        },
        particles: readback.particles,
        grid: readback.grid,
        dispatchArgs: readback.dispatchArgs,
        counters: {
            particleCount: readback.particleCount,
            freeCount: readback.freeCount,
        },
        raw: {
            particleBufferFloats: readback.particleFloats,
        },
    };

    mkdirSync(dirname(OUT_PATH), {recursive: true});
    writeFileSync(OUT_PATH, JSON.stringify(trace));

    report(trace, OUT_PATH);
}

function lastBufferLabelled(label)
{
    const matches = createdBuffers.filter((buffer) => buffer.label === label);
    return matches.length ? matches[matches.length - 1] : null;
}

// ------------------------------------------------------------------
// inputs
// ------------------------------------------------------------------

function buildInputs()
{
    return {
        // geometry / domain
        gridSize: GRID_SIZE,
        resolution: RESOLUTION,
        simResDivisor: SIM_RES_DIVISOR,

        // per-frame time control (see time.doTimeRegulation)
        timeStamp: 0,
        doReset: true,
        doPause: false,

        // mouse (inactive)
        isMouseDown: false,
        mousePosition: [RESOLUTION[0] / 2, RESOLUTION[1] / 2],
        mousePrevPosition: [RESOLUTION[0] / 2, RESOLUTION[1] / 2],
        mouseRadius: 100,
        mouseFunction: sim.SimEnums.MouseFunctionGrab,

        // solver settings (upstream ui.js defaults except iterationCount/simRate)
        simRate: 120,
        iterationCount: 2,
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

        // SDF shapes: empty. constructShapeBuffer emits one inert filler shape so
        // the storage binding is non-null; shapeCount (inputs.shapes.size) is 0.
        shapes: new Set(),
    };
}

function inputsConfig(inputs)
{
    const inputsOut = {
        gridSize: inputs.gridSize,
        resolution: inputs.resolution,
        simResDivisor: inputs.simResDivisor,
        simRate: inputs.simRate,
        deltaTime: 1.0 / inputs.simRate,
        iterationCount: inputs.iterationCount,
        particlesPerCellAxis: inputs.particlesPerCellAxis,
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
        mouseRadiusInGridUnits: inputs.mouseRadius / inputs.simResDivisor,
        mouseFunction: inputs.mouseFunction,
        isMouseDown: inputs.isMouseDown,
        shapeCount: inputs.shapes.size,
        timeStamp: inputs.timeStamp,
        doReset: inputs.doReset,
    };

    const provenance = {
        gridSize: "harness decision (32x32); upstream derives floor(canvas/simResDivisor) in ui.windowResize",
        resolution: "harness decision = gridSize * simResDivisor (256x256)",
        simResDivisor: "ui.js g_uiElements default 8",
        simRate: "harness decision 120 (ui.js default 240); 120 with a 60 Hz frame delta yields exactly 2 substeps in one update() so the traced step is the first simulated substep",
        iterationCount: "task requirement 2 (ui.js default 5, damBreak.json override 10)",
        particlesPerCellAxis: "harness decision 1 (ui.js default 2) - one particle per seeded cell",
        rhoZeroMultiplier: "harness decision 1.0; sim.js computes rho_zero from it but rho_zero is not a declared SimConstants member, so it never reaches the GPU",
        fixedPointMultiplierExponent: "ui.js g_uiElements default 7",
        useGridVolumeForLiquid: "ui.js g_uiElements default true (1)",
        gravityStrength: "ui.js g_uiElements default 2.5",
        liquidViscosity: "ui.js g_uiElements default 0.01",
        liquidRelaxation: "ui.js g_uiElements default 1.5",
        elasticRelaxation: "ui.js g_uiElements default 1.5 (unused by Liquid)",
        elasticityRatio: "ui.js g_uiElements default 1 (unused by Liquid)",
        frictionAngle: "ui.js g_uiElements default 30 (unused by Liquid)",
        plasticity: "ui.js g_uiElements default 0.9 (unused by Liquid)",
        borderFriction: "ui.js g_uiElements default 0.5",
        mouseRadius: "main.js g_mouseRadius = 100 render px (inactive: isMouseDown false)",
        mouseFunction: "ui.js default MouseFunctionGrab",
        shapes: "harness decision: empty Set; upstream constructs one inert filler SimShape when shapeCount is 0",
        material: "SimEnums.MaterialLiquid = 0 (src/sim.js)",
    };

    return {inputs: inputsOut, provenance};
}

// ------------------------------------------------------------------
// deterministic seed
// ------------------------------------------------------------------

function buildSeedParticles()
{
    const particles = [];

    for(let y = 0; y < BLOCK_SIZE[1]; ++y)
    {
        for(let x = 0; x < BLOCK_SIZE[0]; ++x)
        {
            particles.push({
                position: [BLOCK_ORIGIN[0] + x + 0.5, BLOCK_ORIGIN[1] + y + 0.5],
                displacement: [0, 0],
                deformationGradient: [1, 0, 0, 1],
                deformationDisplacement: [0, 0, 0, 0],
                liquidDensity: 1.0,
                mass: 1.0,
                material: sim.SimEnums.MaterialLiquid,
                volume: 1.0,
                lambda: 0.0,
                logJp: 1.0,
                color: [0.2, 0.2, 1.0],
                enabled: 1.0,
            });
        }
    }

    return {particles};
}

// ------------------------------------------------------------------
// readback
// ------------------------------------------------------------------

async function readBackAll(device, gpuContext, options)
{
    const particleCount = options.particleCount;
    const particleBytes = gpuContext.maxParticleCount * gpu.ParticleByteStride;
    const gridBytes = GRID_SIZE[0] * GRID_SIZE[1] * 4 * 4;
    const bukkitThreadDataBytes = gridBytes;
    const dispatchArgsBytes = 16;

    const staging = device.createBuffer({
        label: "particleStaging",
        size: particleBytes,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    const gridStaging = options.gridBuffers.map((buffer, index) => device.createBuffer({
        label: `gridStaging${index}`,
        size: gridBytes,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    }));

    const counterStaging = device.createBuffer({
        label: "counterStaging",
        size: 16,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    const bukkitDispatchStaging = device.createBuffer({
        label: "bukkitDispatchStaging",
        size: dispatchArgsBytes,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    const simDispatchStaging = device.createBuffer({
        label: "simDispatchStaging",
        size: dispatchArgsBytes,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    const threadDataStaging = device.createBuffer({
        label: "threadDataStaging",
        size: bukkitThreadDataBytes,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    const encoder = device.createCommandEncoder({label: "readback"});
    encoder.copyBufferToBuffer(gpuContext.particleBuffer, 0, staging, 0, particleBytes);
    for(let i = 0; i < options.gridBuffers.length; ++i)
    {
        encoder.copyBufferToBuffer(options.gridBuffers[i], 0, gridStaging[i], 0, gridBytes);
    }
    encoder.copyBufferToBuffer(gpuContext.particleCountBuffer, 0, counterStaging, 0, 4);
    encoder.copyBufferToBuffer(gpuContext.particleFreeIndicesBuffer, 0, counterStaging, 4, 4);
    encoder.copyBufferToBuffer(gpuContext.particleSimDispatchBuffer, 0, simDispatchStaging, 0, dispatchArgsBytes);

    if(options.bukkitDispatchBuffer)
    {
        encoder.copyBufferToBuffer(options.bukkitDispatchBuffer, 0, bukkitDispatchStaging, 0, dispatchArgsBytes);
    }
    if(options.bukkitThreadDataBuffer)
    {
        const bytes = Math.min(bukkitThreadDataBytes, options.bukkitThreadDataBuffer.size);
        encoder.copyBufferToBuffer(options.bukkitThreadDataBuffer, 0, threadDataStaging, 0, bytes);
    }

    device.queue.submit([encoder.finish()]);
    await device.queue.onSubmittedWorkDone();

    await staging.mapAsync(GPUMapMode.READ, 0, particleCount * gpu.ParticleByteStride);
    const particleFloats = new Float32Array(staging.getMappedRange(0, particleCount * gpu.ParticleByteStride)).slice();
    staging.unmap();

    const grid = [];
    for(let i = 0; i < options.gridBuffers.length; ++i)
    {
        await gridStaging[i].mapAsync(GPUMapMode.READ, 0, gridBytes);
        grid.push({
            label: options.gridBuffers[i].label,
            values: Array.from(new Int32Array(gridStaging[i].getMappedRange(0, gridBytes))),
        });
        gridStaging[i].unmap();
    }

    await counterStaging.mapAsync(GPUMapMode.READ, 0, 8);
    const counters = new Int32Array(counterStaging.getMappedRange(0, 8)).slice();
    counterStaging.unmap();

    await simDispatchStaging.mapAsync(GPUMapMode.READ, 0, dispatchArgsBytes);
    const simDispatchArgs = Array.from(new Uint32Array(simDispatchStaging.getMappedRange(0, dispatchArgsBytes)));
    simDispatchStaging.unmap();

    let bukkitDispatchArgs = null;
    if(options.bukkitDispatchBuffer)
    {
        await bukkitDispatchStaging.mapAsync(GPUMapMode.READ, 0, dispatchArgsBytes);
        bukkitDispatchArgs = Array.from(new Uint32Array(bukkitDispatchStaging.getMappedRange(0, dispatchArgsBytes)));
        bukkitDispatchStaging.unmap();
    }

    // BukkitThreadData is {rangeStart, rangeCount, bukkitX, bukkitY}, 16 bytes each.
    let bukkitThreadData = null;
    if(options.bukkitThreadDataBuffer)
    {
        const bytes = Math.min(bukkitThreadDataBytes, options.bukkitThreadDataBuffer.size);
        await threadDataStaging.mapAsync(GPUMapMode.READ, 0, bytes);
        const words = new Uint32Array(threadDataStaging.getMappedRange(0, bytes));
        bukkitThreadData = [];
        for(let i = 0; i + 3 < words.length; i += 4)
        {
            const entry = [words[i], words[i + 1], words[i + 2], words[i + 3]];
            if(entry[1] !== 0)
            {
                bukkitThreadData.push({rangeStart: entry[0], rangeCount: entry[1], bukkitX: entry[2], bukkitY: entry[3]});
            }
        }
        threadDataStaging.unmap();
    }

    const particles = [];
    for(let i = 0; i < particleCount; ++i)
    {
        const o = i * gpu.ParticleFloatCount;
        particles.push({
            index: i,
            position: [particleFloats[o + 0], particleFloats[o + 1]],
            displacement: [particleFloats[o + 2], particleFloats[o + 3]],
            deformationGradient: [particleFloats[o + 4], particleFloats[o + 5], particleFloats[o + 6], particleFloats[o + 7]],
            deformationDisplacement: [particleFloats[o + 8], particleFloats[o + 9], particleFloats[o + 10], particleFloats[o + 11]],
            liquidDensity: particleFloats[o + 12],
            mass: particleFloats[o + 13],
            material: particleFloats[o + 14],
            volume: particleFloats[o + 15],
            lambda: particleFloats[o + 16],
            logJp: particleFloats[o + 17],
            color: [particleFloats[o + 20], particleFloats[o + 21], particleFloats[o + 22]],
            enabled: particleFloats[o + 23],
        });
    }

    return {
        particles,
        particleFloats: Array.from(particleFloats),
        grid,
        particleCount: counters[0],
        freeCount: counters[1],
        dispatchArgs: {
            particleSim: simDispatchArgs,
            bukkit: bukkitDispatchArgs,
            bukkitThreadData,
        },
    };
}

// ------------------------------------------------------------------
// verification
// ------------------------------------------------------------------

function verify(seedParticles, readback, inputs)
{
    const nonFiniteDetails = [];

    const fieldValues = (particle) => [
        ["position", particle.position],
        ["displacement", particle.displacement],
        ["deformationGradient", particle.deformationGradient],
        ["deformationDisplacement", particle.deformationDisplacement],
        ["liquidDensity", [particle.liquidDensity]],
        ["mass", [particle.mass]],
        ["volume", [particle.volume]],
        ["lambda", [particle.lambda]],
        ["logJp", [particle.logJp]],
        ["color", particle.color],
        ["enabled", [particle.enabled]],
    ];

    for(const particle of readback.particles)
    {
        for(const [field, components] of fieldValues(particle))
        {
            components.forEach((value, component) =>
            {
                if(!Number.isFinite(value))
                {
                    nonFiniteDetails.push({index: particle.index, field, component, value: String(value)});
                }
            });
        }
    }

    const nonFinite = nonFiniteDetails.length;

    let minDisplacementY = Infinity;
    let maxDisplacementY = -Infinity;
    let maxAbsDisplacement = 0;
    let maxDetDeviation = 0;
    let maxDfDetDeviation = 0;
    let minJ = Infinity;
    let maxJ = -Infinity;
    let maxPositionDelta = 0;

    for(let i = 0; i < readback.particles.length; ++i)
    {
        const particle = readback.particles[i];
        const seedParticle = seedParticles[i];

        minDisplacementY = Math.min(minDisplacementY, particle.displacement[1]);
        maxDisplacementY = Math.max(maxDisplacementY, particle.displacement[1]);
        maxAbsDisplacement = Math.max(maxAbsDisplacement, Math.abs(particle.displacement[0]), Math.abs(particle.displacement[1]));

        const f = particle.deformationGradient;
        const detF = f[0] * f[3] - f[2] * f[1];
        maxDetDeviation = Math.max(maxDetDeviation, Math.abs(detF - 1));

        const d = particle.deformationDisplacement;
        const detD = d[0] * d[3] - d[2] * d[1];
        maxDfDetDeviation = Math.max(maxDfDetDeviation, Math.abs(detD));

        // Liquid integrates volume in liquidDensity instead of the deformation gradient.
        minJ = Math.min(minJ, particle.liquidDensity);
        maxJ = Math.max(maxJ, particle.liquidDensity);

        maxPositionDelta = Math.max(maxPositionDelta,
            Math.abs(particle.position[0] - seedParticle.position[0]),
            Math.abs(particle.position[1] - seedParticle.position[1]));
    }

    // The final grid buffers prove g2p2g actually ran over the particles:
    // mass words (component 2) can only be produced by the P2G pass.
    const gridMasses = readback.grid.map((buffer) =>
    {
        let massSum = 0;
        let momentumXSum = 0;
        let momentumYSum = 0;
        for(let i = 0; i < buffer.values.length; i += 4)
        {
            momentumXSum += buffer.values[i];
            momentumYSum += buffer.values[i + 1];
            massSum += buffer.values[i + 2];
        }
        return {label: buffer.label, encodedMass: massSum, encodedMomentumX: momentumXSum, encodedMomentumY: momentumYSum};
    });

    const fixedPointMultiplier = Math.ceil(Math.pow(10, inputs.fixedPointMultiplierExponent));
    const gridMassInParticles = Math.max(...gridMasses.map((entry) => entry.encodedMass)) / fixedPointMultiplier;

    return {
        finiteValues: nonFinite === 0,
        nonFiniteCount: nonFinite,
        nonFiniteValues: nonFiniteDetails,
        particleCountReadBack: readback.particleCount,
        freeCountReadBack: readback.freeCount,
        displacementY: {
            min: minDisplacementY,
            max: maxDisplacementY,
            allNegative: maxDisplacementY < 0,
            expectedGravityDeltaPerSubstep: -GRID_SIZE[1] * inputs.gravityStrength * Math.pow(1 / inputs.simRate, 2),
            note: "g2p2g applies gravity once, on the last iteration, and does position += displacement before it, so a single substep from rest changes velocity but not position",
        },
        maxAbsDisplacement,
        maxPositionDeltaFromSeed: maxPositionDelta,
        deformationGradientDet: {
            maxAbsDeviationFromOne: maxDetDeviation,
        },
        deformationDisplacementDet: {
            maxAbsDeterminant: maxDfDetDeviation,
        },
        liquidDensity: {
            min: minJ,
            max: maxJ,
            nearOne: Math.abs(minJ - 1) < 0.05 && Math.abs(maxJ - 1) < 0.05,
        },
        grid: gridMasses,
        gridMassInParticles,
        indirectDispatchArgs: readback.dispatchArgs,
        webgpuValidationErrors: 0,
    };
}

// ------------------------------------------------------------------
// upstream metadata
// ------------------------------------------------------------------

function upstreamCommit()
{
    // The vendored tree lives inside this repository and has no git history of
    // its own, so probing it would report the host repository's HEAD. The pin is
    // the authoritative answer there; an external checkout is probed normally.
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

// Recorded provenance stays portable: the vendored tree is named by its
// repo-relative location, not by the absolute path of the current checkout.
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
// report
// ------------------------------------------------------------------

function report(trace, outPath)
{
    const verification = trace.meta.verification;
    const frames = trace.meta.frames;

    console.log(`upstream commit          : ${trace.meta.upstream.commit}`);
    console.log(`adapter                  : ${trace.meta.runtime.adapterVendor} / ${trace.meta.runtime.adapterDevice}`);
    console.log(`grid / resolution        : ${GRID_SIZE[0]}x${GRID_SIZE[1]} / ${RESOLUTION[0]}x${RESOLUTION[1]}`);
    console.log(`frames / iterationCount  : ${frames.length} / ${trace.meta.inputs.iterationCount}`);
    console.log(`particles seeded/traced  : ${trace.meta.seed.particleCount} / ${trace.particles.length}`);
    console.log(`dispatches (last frame)  : ${frames[frames.length - 1].dispatchLog.map((entry) => `${entry.shader}(${entry.dispatch.mode})`).join(", ")}`);
    console.log(`indirect sim dispatch    : [${trace.dispatchArgs.particleSim.join(", ")}]  bukkit: [${trace.dispatchArgs.bukkit?.join(", ") ?? "n/a"}]`);
    console.log(`bukkit work items        : ${JSON.stringify(trace.dispatchArgs.bukkitThreadData)}`);
    console.log(`finite                   : ${verification.finiteValues} (non-finite ${verification.nonFiniteCount})`);

    if(verification.nonFiniteCount !== 0)
    {
        console.log(`NON-FINITE VALUES        : ${JSON.stringify(verification.nonFiniteValues)}`);
    }
    console.log(`displacement.y range     : [${verification.displacementY.min}, ${verification.displacementY.max}]`);
    console.log(`max |position - seed|    : ${verification.maxPositionDeltaFromSeed}`);
    console.log(`|det(F) - 1| max         : ${verification.deformationGradientDet.maxAbsDeviationFromOne}`);
    console.log(`liquidDensity range      : [${verification.liquidDensity.min}, ${verification.liquidDensity.max}]`);
    console.log(`grid mass (particles)    : ${verification.gridMassInParticles}`);
    console.log(`counters                 : particleCount=${trace.counters.particleCount} free=${trace.counters.freeCount}`);
    console.log(`trace written            : ${outPath}`);
}

try
{
    await main();
}
catch(error)
{
    fail(error?.stack ?? String(error));
}
