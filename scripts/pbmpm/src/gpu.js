//-----------------------------------------------------------------------------
// MODIFIED FILE — headless replacement for upstream src/gpu.js.
//
// This is NOT an upstream file. Upstream src/gpu.js (DOM, canvas, render
// pipeline, timestamp queries) is deliberately not vendored; this compute-only
// replacement lives at scripts/pbmpm/src/gpu.js and is wired into the verbatim
// upstream sim.js through the resolution shim upstream/src/gpu.js.
//
// Exposes exactly the surface upstream src/sim.js imports and uses:
//   divUp, construct4IntBuffer, computeDispatch, beginFrame, endFrame,
//   plus a context object (getGpuContext).
//
// Differences from upstream src/gpu.js:
//   1. Compute-only subset: no navigator.gpu, no document, no canvas configure,
//      no render pipeline, no timestamp queries, no particle render buffer.
//   2. Fixed domains instead of canvas-derived ones: maxParticleCount is 4096
//      (upstream: 1,000,000) and the callers pick the grid/resolution.
//   3. Snapshot hooks for the trace writers: context.frameCopies collects the
//      per-substep particle/grid copies that endFrame() inserts into the frame's
//      own command buffer, so a trace can snapshot state without a second
//      submission.
//   4. Indirect-args probes: before every indirect dispatch the 16-byte
//      arguments buffer is copied into context.probeTarget at a running offset
//      and recorded in context.frameProbes, so a trace can prove the workgroup
//      count each dispatch actually ran with.
//   5. context.dispatchLog records every compute dispatch of the frame for
//      evidence, and the particle buffer is STORAGE|COPY_DST (the runners add
//      COPY_SRC through instrumentation) so it can be read back.
//
// computeDispatch keeps upstream binding semantics:
//   - a flat array of GPUBuffers  -> bindings 0..n-1 in one bind group
//   - an array of arrays          -> one bind group per array, bindings 0..n-1
//   - groupCount array            -> dispatchWorkgroups(x,y,z)
//   - groupCount number           -> dispatchWorkgroupsIndirect(buffer, 0)
//-----------------------------------------------------------------------------

"use strict";

// Upstream shader.js is vendored verbatim next to the upstream modules.
import * as shader from "../upstream/src/shader.js"

let context = {
    pipelines: {},

    // Upstream default is 1000000 particles. The harness keeps this small so the
    // particle buffer, bukkit particle data and readback staging stay cheap.
    // Must stay >= the seeded particle count.
    maxParticleCount: 4096,

    encoder: null,

    // Particle counts are only maintained for reporting; sim.js itself never
    // reads them (it reads particleCountBuffer on the GPU).
    particleCount: 0,
    particleFreeCount: 0,

    // Populated by the harness after each submitted frame.
    pendingSubmit: null,

    // Ordered record of every compute dispatch issued this frame, for evidence.
    dispatchLog: [],

    // Harness instrumentation, used by stage 2 (multi-substep traces).
    //
    // frameCopies: copy commands inserted into the frame's command buffer by
    // endFrame() (per-step particle/grid snapshots). Each entry is
    // {src, dst, srcOffset?, dstOffset?, size}.
    //
    // probeTarget/frameProbes: before every indirect dispatch the 16-byte
    // indirect args buffer is copied into probeTarget at a running offset, so
    // the trace can prove the workgroup counts each dispatch actually ran with
    // (including the zero-work bootstrap substep).
    frameCopies: [],
    probeTarget: null,
    probeOffset: 0,
    frameProbes: [],

    // Number of consecutive g2p2g dispatches in the current run. sim.js issues
    // exactly inputs.iterationCount of them per substep; the harness sets
    // expectedSubstepIterations from the inputs it passes in.
    g2p2gRunLength: 0,
    expectedSubstepIterations: 0,
};

// Must be kept in sync with Particle in shaders/particle.inc.wgsl.
export const ParticleFloatCount = 24;
export const ParticleByteStride = ParticleFloatCount * 4;

export function getGpuContext() {return context;}

export function divUp(threadCount, divisor)
{
    return Math.floor((threadCount + divisor - 1) / divisor);
}

// Initialize the compute side of the device. Mirrors upstream gpu.init's
// device/shader steps without the canvas/render-pipeline parts.
export async function init(device, insertHandlers)
{
    context.insertHandlers = insertHandlers;
    context.device = device;
    context.canTimeStamp = false;

    await shader.init(device, insertHandlers);
}

export function construct4IntBuffer(name, usage, values)
{
    const buf = context.device.createBuffer({
        label: name,
        size: 16,
        usage: usage
    })

    const valueArray = new Int32Array(4);
    valueArray.set(values);
    context.device.queue.writeBuffer(buf, 0, valueArray);

    return buf;
}

export function resetBuffers(gridSize)
{
    // Construct various small buffers used for indirect dispatch, counting and staging
    context.particleCountBuffer = construct4IntBuffer('particleCountBuffer', GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC, [0,0,0,0]);
    context.particleRenderDispatchBuffer = construct4IntBuffer('particleRenderDispatchBuffer', GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.INDIRECT, [6,0,0,0]);
    context.particleSimDispatchBuffer = construct4IntBuffer('particleSimDispatchBuffer', GPUBufferUsage.STORAGE | GPUBufferUsage.INDIRECT | GPUBufferUsage.COPY_DST, [0,1,1,0]);

    // Construct particle buffer.
    // Must be kept in sync with MPMParticle in particle.inc.wgsl
    context.particleBuffer = context.device.createBuffer({
        label: "particles",
        size: context.maxParticleCount * ParticleByteStride,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
    });

    context.particleFreeIndicesBuffer = context.device.createBuffer({
        label: 'freeIndices',
        size: 4 + context.maxParticleCount * 4,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST
    });

    context.gridSize = gridSize;
}

// Seed the particle storage buffer directly, using the MPMParticle field order
// from shaders/particle.inc.wgsl. Particles are packed contiguously at the
// shader's array<Particle> stride (96 bytes / 24 floats per particle).
export function seedParticles(particles, particleCount)
{
    if(particleCount > context.maxParticleCount)
    {
        throw `seedParticles: ${particleCount} particles exceeds maxParticleCount ${context.maxParticleCount}`;
    }

    const floats = new Float32Array(particleCount * ParticleFloatCount);

    for(let i = 0; i < particleCount; ++i)
    {
        const p = particles[i];
        const base = i * ParticleFloatCount;

        floats[base + 0] = p.position[0];
        floats[base + 1] = p.position[1];

        floats[base + 2] = p.displacement[0];
        floats[base + 3] = p.displacement[1];

        // mat2x2f is column-major in WGSL memory layout: (col0.x, col0.y, col1.x, col1.y)
        floats[base + 4] = p.deformationGradient[0];
        floats[base + 5] = p.deformationGradient[1];
        floats[base + 6] = p.deformationGradient[2];
        floats[base + 7] = p.deformationGradient[3];

        floats[base + 8] = p.deformationDisplacement[0];
        floats[base + 9] = p.deformationDisplacement[1];
        floats[base + 10] = p.deformationDisplacement[2];
        floats[base + 11] = p.deformationDisplacement[3];

        floats[base + 12] = p.liquidDensity;
        floats[base + 13] = p.mass;
        floats[base + 14] = p.material;
        floats[base + 15] = p.volume;

        floats[base + 16] = p.lambda;
        floats[base + 17] = p.logJp;

        // floats 18, 19 are the 16-byte alignment padding before color (vec3f)
        floats[base + 20] = p.color[0];
        floats[base + 21] = p.color[1];
        floats[base + 22] = p.color[2];

        floats[base + 23] = p.enabled;
    }

    context.device.queue.writeBuffer(context.particleBuffer, 0, floats);

    // Word 0 of the particle count buffer gates every particle dispatch.
    context.device.queue.writeBuffer(
        context.particleCountBuffer, 0, new Uint32Array([particleCount, 0, 0, 0]));

    // Word 0 of the free list is the free count consumed by particleEmit /
    // g2p2g (atomicSub / atomicMax). Start with an empty free list.
    context.device.queue.writeBuffer(
        context.particleFreeIndicesBuffer, 0, new Uint32Array([0, 0, 0, 0]));

    context.particleCount = particleCount;
    context.particleFreeCount = 0;
}

export function beginFrame()
{
    context.dispatchLog = [];
    context.frameCopies = [];
    context.frameProbes = [];
    context.g2p2gRunLength = 0;
    context.encoder = context.device.createCommandEncoder();
}

// Ends the frame: appends the harness-requested snapshot copies, submits the
// recorded command buffer and returns a promise that resolves once the GPU has
// finished the submission. Upstream endFrame also kicks off asynchronous
// particle-count/timestamp readbacks; the harness reads all buffers back
// explicitly after this resolves.
export function endFrame()
{
    for(const copy of context.frameCopies)
    {
        context.encoder.copyBufferToBuffer(
            copy.src, copy.srcOffset ?? 0, copy.dst, copy.dstOffset ?? 0, copy.size);
    }

    context.device.queue.submit([context.encoder.finish()]);
    context.encoder = null;

    context.pendingSubmit = context.device.queue.onSubmittedWorkDone();
    return context.pendingSubmit;
}

// Helper function to dispatch a compute shader with the given name, resources
// and dispatch size.
// if GroupCount is an array then a regular dispatch is done, otherwise
// it is bound as an indirect buffer and an indirect dispatch is done.
export function computeDispatch(shaderName, resources, groupCount)
{
    let entries = []

    var isSimpleFlatBufferList = true;
    var isNestedBufferList = true;
    for(let i = 0; i < resources.length; ++i)
    {
        if(!resources[i])
        {
            throw `Compute Dispatch [${shaderName}]: Resource at index ${i} was falsy!`
        }
        if(!(resources[i] instanceof GPUBuffer))
        {
            isSimpleFlatBufferList = false;
        }
        if(Array.isArray(resources[i]))
        {
            for(let j = 0; j < resources[i].length; ++j)
            {
                if(!resources[i][j])
                {
                    throw `Compute Dispatch [${shaderName}]: Resource at group ${i} index ${j} was falsy!`
                }
                if(!(resources[i][j] instanceof GPUBuffer))
                {
                    isNestedBufferList = false;
                }
            }
        }
    }

    if(!isSimpleFlatBufferList && !isNestedBufferList)
    {
        throw `Expected resources to be an array of resources OR an array of arrays of resources.`;
    }

    if(isSimpleFlatBufferList)
    {
        entries.push([])
        for(let i = 0; i < resources.length; ++i)
        {
            entries[0].push({binding: i, resource: {buffer: resources[i]}});
        }
    }
    else if(isNestedBufferList)
    {
        for(let i = 0; i < resources.length; ++i)
        {
            let thisGroupEntries = []

            for(let j = 0; j < resources[i].length; ++j)
            {
                thisGroupEntries.push({binding: j, resource: {buffer: resources[i][j]}});
            }

            entries.push(thisGroupEntries);
        }
    }

    const pipeline = shader.getComputePipeline(shaderName);

    // Instrumentation: record the indirect args this dispatch is about to use.
    // Copies execute in submission order, so this reads the buffer exactly as
    // the dispatch will see it (including the blank [0,1,1,0] bootstrap args).
    let probeOffset = null;
    if(!Array.isArray(groupCount) && context.probeTarget)
    {
        if(context.probeOffset + 16 <= context.probeTarget.size)
        {
            probeOffset = context.probeOffset;
            context.encoder.copyBufferToBuffer(groupCount, 0, context.probeTarget, probeOffset, 16);
            context.probeOffset += 16;
        }
    }

    const computePass = context.encoder.beginComputePass({label: shaderName});
    computePass.setPipeline(pipeline);

    for(let i = 0; i < entries.length; ++i)
    {
        const bindGroup = context.device.createBindGroup({
            layout: pipeline.getBindGroupLayout(0),
            entries: entries[i]});
        computePass.setBindGroup(i, bindGroup);
    }

    if(Array.isArray(groupCount))
    {
        computePass.dispatchWorkgroups(groupCount[0], groupCount[1], groupCount[2]);
    }
    else
    {
        computePass.dispatchWorkgroupsIndirect(groupCount, 0);
    }
    computePass.end()

    if(shaderName === 'g2p2g')
    {
        context.g2p2gRunLength += 1;
    }
    else
    {
        context.g2p2gRunLength = 0;
    }

    context.frameProbes.push({
        shader: shaderName,
        probeOffset,
        probeWord: probeOffset === null ? -1 : probeOffset / 16,
        // 1-based position within the current g2p2g run (0 for other shaders):
        // the last one (== expectedSubstepIterations) is the substep's final
        // iteration, whose P2G output lands in the grid buffers.
        runIndex: shaderName === 'g2p2g' ? context.g2p2gRunLength : 0,
    });

    context.dispatchLog.push({
        shader: shaderName,
        bindings: resources.length,
        dispatch: Array.isArray(groupCount) ? {mode: 'direct', counts: groupCount.slice()} : {mode: 'indirect', buffer: groupCount.label},
    });
}
