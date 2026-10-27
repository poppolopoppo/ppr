module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>
#include <cstdio>

module engine.tests.app;
import std;
import engine.core;
import engine.shader;
import engine.rhi;
import engine.matter;

namespace pP::tests::detail::MatterFork {
    using matter::GpuParameters;
    using matter::GpuStaticShape;
    using matter::SourceMatrix;
}

#include "App.MatterFork.Oracle.h"

namespace pP::tests::detail::MatterFork {
    // ------------------------------------------------------------------
    // private reflected split ABI and direct native fixture
    // ------------------------------------------------------------------
    struct Hot {
        float px, py, dx, dy;
        SourceMatrix F, D;
        float J, logJp, lambda, enabled;
    };
    struct Cold {
        float mass, volume, material, r, g, b;
    };
    static_assert(sizeof(Hot) == 64u and alignof(Hot) == 4u);
    static_assert(sizeof(Cold) == 24u and alignof(Cold) == 4u);
    static_assert(PPR_OFFSETOF(Hot, logJp) == 52u and PPR_OFFSETOF(Hot, lambda) == 56u);
    static_assert(PPR_OFFSETOF(Cold, r) == 12u and PPR_OFFSETOF(Cold, b) == 20u);

    // The frozen WGSL oracle keeps its original eight-word representation.
    // Translate only its two proven-zero padding words at the device boundary.
    std::array<u32, 6u> compactCold(const u32 *const words) {
        PPR_TEST_ASSERT(words[3u] == 0u and words[7u] == 0u);
        return {words[0u], words[1u], words[2u], words[4u], words[5u], words[6u]};
    }

    std::array<u32, 8u> oracleCold(const u32 *const words) {
        return {words[0u], words[1u], words[2u], 0u, words[3u], words[4u], words[5u], 0u};
    }

    struct Fixture {
        safe_ptr<IShaderService> shaders{IShaderService::get()};
        safe_ptr<IRhiService> rhi_service{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> queue;
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 4> programs;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 4> pipelines;
        bool shaders_attempted{}, rhi_attempted{};

        void initialize() {
            shaders_attempted = true;
            PPR_TEST_ASSERT(not shaders->initialize());

            rhi_service->setCoreValidation(true);
            rhi_service->setGpuAssistedValidation(true);
            rhi_attempted = true;
            PPR_TEST_ASSERT(not rhi_service->initialize(rhi::DeviceType::D3D12, *shaders));

            auto &device = rhi_service->getDevice();
            PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, queue.writeRef())));

            shader::SharedModule module;
            PPR_TEST_ASSERT(not shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "pbmpm" / "fork_conformance.slang", "fork_conformance",
                module.writeRef()));
            auto *layout = module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            for (const auto &[name, size]:
                std::array<std::pair<const char *, std::size_t>, 2>{{{"g_hot", 64u}, {"g_cold", 24u}}}) {
                slang::TypeLayoutReflection *element{};
                for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
                    auto *parameter = layout->getParameterByIndex(i);
                    if (std::string_view{parameter->getName()} == name)
                        element = parameter->getTypeLayout()->getElementTypeLayout();
                }

                PPR_TEST_ASSERT(element != nullptr);
                PPR_TEST_ASSERT(element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == size);
                const char *member = std::string_view{name} == "g_hot" ? "logJp" : "r";
                const std::size_t expected_offset = std::string_view{name} == "g_hot" ? 52u : 12u;
                bool found = false;
                for (unsigned i = 0; i < element->getFieldCount(); ++i) {
                    auto *field = element->getFieldByIndex(i);
                    if (std::string_view{field->getName()} == member) {
                        PPR_TEST_ASSERT(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == expected_offset);
                        found = true;
                    }
                }

                PPR_TEST_ASSERT(found);
            }

            const std::array names{"countBuckets", "allocateBuckets", "insertBuckets", "fused"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                shader::ComPtr<slang::IEntryPoint> entry;
                PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(names[i], entry.writeRef())));

                slang::IComponentType *entries[]{entry.get()};
                rhi::ShaderProgramDesc desc{};
                desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                desc.slangGlobalScope = module.get();
                desc.slangEntryPoints = entries;
                desc.slangEntryPointCount = 1u;
                shader::Diagnose diagnostics;
                PPR_TEST_ASSERT(not make_error_code(
                    device.createShaderProgram(desc, programs[i].writeRef(), diagnostics.writeRef())));

                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = programs[i];
                PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline, pipelines[i].writeRef())));
            }
        }

        void shutdown() {
            if (queue and make_error_code(queue->waitOnHost()))
                hal::fatalExit("fork-conformance-completion-unknown");

            pipelines = {};
            programs = {};
            queue.setNull();
            const auto rhi_error = rhi_attempted ? rhi_service->shutdown() : std::error_code{};
            const auto shader_error = shaders_attempted ? shaders->shutdown() : std::error_code{};
            PPR_TEST_ASSERT(not rhi_error);
            PPR_TEST_ASSERT(not shader_error);
        }

        rhi::ComPtr<rhi::IBuffer> buffer(const u64 bytes, const u32 stride, const void *data = nullptr) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = stride;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource) |
                static_cast<u32>(rhi::BufferUsage::CopyDestination) |
                static_cast<u32>(rhi::BufferUsage::IndirectArgument));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            rhi::ComPtr<rhi::IBuffer> result;
            PPR_TEST_ASSERT(not make_error_code(rhi_service->getDevice().createBuffer(desc, data, result.writeRef())));
            return result;
        }

        void submit(rhi::ICommandEncoder &encoder) {
            rhi::ComPtr<rhi::ICommandBuffer> commands;
            PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));

            bool complete = false;
            PPR_DEFER {
                if (not complete and make_error_code(queue->waitOnHost()))
                    hal::fatalExit("fork-conformance-submit-unknown");
            };
            PPR_TEST_ASSERT(not make_error_code(queue->submit(commands)));
            PPR_TEST_ASSERT(not make_error_code(queue->waitOnHost()));

            complete = true;
        }
    };

    // ------------------------------------------------------------------
    // exact stage observations through existing RHI ownership
    // ------------------------------------------------------------------
    struct State {
        Fixture &fixture;
        GpuParameters parameters;
        rhi::ComPtr<rhi::IBuffer> hot, cold, shapes, grid[3], budget, errors, counts, inserts, starts, rows, allocator,
            args, buckets;
        u32 grid_index{};

        State(
            Fixture &owner, const OracleCase &input, const u32 *hot_words = nullptr, const u32 *cold_words = nullptr,
            u32 count_override = 0u)
            : fixture(owner), parameters(std::bit_cast<GpuParameters>(input.parameters)) {
            if (count_override != 0u)
                parameters.m_particle_count = count_override;

            const u32 N = parameters.m_particle_count;
            const std::array<u32, 4096> zero{};
            const std::array<u32, 6> status{0u, 0xffffffffu, 0xffffffffu, 0u, 0u, 0u};
            const std::array<u32, 4> dispatch{0u, 1u, 1u, 0u};

            hot = fixture.buffer(N * 64u, 64u, hot_words ? hot_words : input.hot);
            std::vector<u32> packed_cold;
            packed_cold.reserve(N * 6u);
            for (u32 row = 0u; row < N; ++row) {
                const auto packed = compactCold((cold_words ? cold_words : input.cold) + row * 8u);
                packed_cold.insert(packed_cold.end(), packed.begin(), packed.end());
            }

            cold = fixture.buffer(N * 24u, 24u, packed_cold.data());
            shapes = fixture.buffer(sizeof(GpuStaticShape), sizeof(GpuStaticShape), &input.shape);
            for (auto &item: grid)
                item = fixture.buffer(sizeof(zero), sizeof(u32), zero.data());

            budget = fixture.buffer(sizeof(zero), sizeof(u32), zero.data());
            errors = fixture.buffer(sizeof(status), sizeof(u32), status.data());
            counts = fixture.buffer(36u * 4u, 4u, zero.data());
            inserts = fixture.buffer(36u * 4u, 4u, zero.data());
            starts = fixture.buffer(36u * 4u, 4u, zero.data());
            rows = fixture.buffer(N * 4u, 4u, zero.data());
            allocator = fixture.buffer(16u, 4u, zero.data());
            args = fixture.buffer(16u, 4u, dispatch.data());
            buckets = fixture.buffer(360u * 16u, 16u);
        }

        std::vector<u32> read(rhi::IBuffer *buffer) {
            std::vector<u32> words(static_cast<std::size_t>(buffer->getDesc().size / 4u));
            PPR_TEST_ASSERT(not make_error_code(
                fixture.rhi_service->getDevice().readBuffer(buffer, 0u, buffer->getDesc().size, words.data())));
            if (buffer == cold.get()) {
                std::vector<u32> oracle_words;
                oracle_words.reserve(parameters.m_particle_count * 8u);
                for (u32 row = 0u; row < parameters.m_particle_count; ++row) {
                    const auto expanded = oracleCold(words.data() + row * 6u);
                    oracle_words.insert(oracle_words.end(), expanded.begin(), expanded.end());
                }

                return oracle_words;
            }

            return words;
        }

        void encode(rhi::ICommandEncoder &encoder, u32 phase) {
            auto *pass = encoder.beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            PPR_DEFER {
                pass->end();
            };

            auto *root = pass->bindPipeline(fixture.pipelines[phase]);
            PPR_TEST_ASSERT(root != nullptr);
            rhi::ShaderCursor cursor{root}, constants;
            PPR_TEST_ASSERT(not make_error_code(cursor["g_parameters"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));

            const auto bind = [&](const char *name, rhi::IBuffer *resource) {
                PPR_TEST_ASSERT(not make_error_code(
                    cursor[name].setBinding(rhi::Binding(resource, {.offset = 0u, .size = resource->getDesc().size}))));
            };

            if (phase == 0u or phase == 2u or phase == 3u) {
                bind("g_hot", hot);
                bind("g_cold", cold);
            }
            bind("g_errors", errors);
            if (phase == 0u or phase == 1u)
                bind("g_counts", counts);
            if (phase == 1u) {
                bind("g_args", args);
                bind("g_allocator", allocator);
                bind("g_buckets", buckets);
                bind("g_starts", starts);
            }

            if (phase == 2u) {
                bind("g_insertCounts", inserts);
                bind("g_starts", starts);
                bind("g_rows", rows);
            }

            if (phase == 3u) {
                bind("g_shapes", shapes);
                bind("g_gridSrc", grid[grid_index]);
                bind("g_gridDst", grid[(grid_index + 1u) % 3u]);
                bind("g_budget", budget);
                bind("g_rows", rows);
                bind("g_buckets", buckets);
                pass->dispatchComputeIndirect({args.get(), 0u});
            } else {
                pass->dispatchCompute(phase == 1u ? 1u : (parameters.m_particle_count + 63u) / 64u, 1u, 1u);
            }
        }

        void bucketize() {
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.queue->createCommandEncoder(encoder.writeRef())));

            for (auto *resource:
                {hot.get(), errors.get(), counts.get(), inserts.get(), starts.get(), rows.get(), allocator.get(),
                    buckets.get(), args.get()})
                encoder->setBufferState(resource, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(cold, rhi::ResourceState::ShaderResource);
            for (auto *resource: {counts.get(), inserts.get(), allocator.get(), rows.get(), buckets.get()})
                encoder->clearBuffer(resource, {.offset = 0u, .size = resource->getDesc().size});

            const std::array<u32, 4> dispatch{0u, 1u, 1u, 0u};
            encoder->setBufferState(args, rhi::ResourceState::CopyDestination);
            PPR_TEST_ASSERT(
                not make_error_code(encoder->uploadBufferData(args, 0u, sizeof(dispatch), dispatch.data())));
            encoder->setBufferState(args, rhi::ResourceState::UnorderedAccess);

            encoder->globalBarrier();
            for (u32 phase = 0; phase < 3; ++phase) {
                encode(*encoder, phase);
                encoder->globalBarrier();
            }

            fixture.submit(*encoder);
        }

        void checkBucketIdentities(const OracleCase &input) {
            const auto active = read(allocator);
            const auto indices = read(rows);
            PPR_TEST_ASSERT(active[0u] <= input.count);

            std::set<u64> visited;
            for (u32 i = 0; i < active[0u]; ++i) {
                PPR_TEST_ASSERT(indices[i] < input.count);
                PPR_TEST_ASSERT(visited.insert(input.ids[indices[i]]).second);
            }

            for (u32 row = 0; row < input.count; ++row) {
                const bool enabled = input.hot[row * 16u + 15u] != 0u;
                PPR_TEST_ASSERT(visited.contains(input.ids[row]) == enabled);
            }
        }

        void step(u32 iteration) {
            parameters.m_reserved0 = iteration;
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.queue->createCommandEncoder(encoder.writeRef())));

            for (auto *resource:
                {hot.get(), budget.get(), errors.get(), rows.get(), buckets.get(), grid[(grid_index + 1u) % 3u].get()})
                encoder->setBufferState(resource, rhi::ResourceState::UnorderedAccess);
            for (auto *resource: {cold.get(), shapes.get(), grid[grid_index].get()})
                encoder->setBufferState(resource, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(args, rhi::ResourceState::IndirectArgument);
            for (auto *resource: {budget.get(), grid[(grid_index + 1u) % 3u].get()})
                encoder->clearBuffer(resource, {.offset = 0u, .size = resource->getDesc().size});
            encoder->globalBarrier();
            encode(*encoder, 3u);
            fixture.submit(*encoder);

            grid_index = (grid_index + 1u) % 3u;
        }

        void resetGridFrontier() {
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.queue->createCommandEncoder(encoder.writeRef())));

            for (auto &resource: grid) {
                encoder->setBufferState(resource, rhi::ResourceState::UnorderedAccess);
                encoder->clearBuffer(resource, {.offset = 0u, .size = resource->getDesc().size});
            }

            fixture.submit(*encoder);

            grid_index = 0u;
        }
    };

    // ------------------------------------------------------------------
    // derived word bounds (plan §B, user decision 2026-10-22)
    // ------------------------------------------------------------------
    // Hot/cold caps are derived from the exact-IEEE contract census (native-batch1/pbmpm-bounds-census.json):
    // F/logJp/lambda/enabled/cold/bucket are fusion-invariant (bit-exact); the deformationDisplacement block
    // carries a signed fma-vs-mul-add accumulation residual (sub-ULP envelope 4·10·2^-24·max(|P|,|t|) plus a
    // per-encode truncation-quantum feedback term); liquidDensity inherits ulp(tr(D)+1).
    // Batch 2 replaced the per-case census realization counts with uniform mechanism-derived caps (position
    // 8e-6, displacement 5e-7, D 1e-6, liquidDensity 1e-6) after fixture 0's secondHot slot realized one 1-ULP
    // encode-quantum feedback flip (displacementY). Batch 3 keeps a sign flip failing only when both magnitudes
    // exceed the class cap; below that it is logged as bounded divergence with a sign-flip marker. A cap of 0
    // means bit-exact.
    //
    // Batch 5 replaces the single-truncation-quantum feedback allowance of the gather classes with a cumulative,
    // event-counted bound. q = 1e-7 is one fixed-point quantum (fixedPointMultiplier = 1e7); fixture 1's rows have
    // mass = volume = 1. Those classes are the only words that consume a quantized encode, so only they count
    // events:
    // - D = 4·B and B = Σ wᵢwⱼ·d·offset with Σ wᵢwⱼ = 1 and |offset| <= 1.5, so one decode-quantum perturbation of
    //   the gathered d moves a D entry by at most 4·1.5·q = 6.0e-7. The gather normalizes, so the grid's
    //   contributors × allowance form must not be applied here: Σ w = 1 replaces the sum over contributors.
    // - Displacement d is fully replaced by the PIC gather, so one gather carries one decode quantum q = 1e-7.
    // - Position is a plain float addition (position += d), never quantized: realized 1 ULP of ~8.5 (9.54e-7),
    //   flat across stages, so it keeps the flat 8e-6.
    // - liquidDensity ← liquidDensity·(tr(D)+1) is a float multiply, never quantized; its realized divergence is
    //   flat across all 16 stages (a state function), so it keeps the flat 1e-6.
    //
    // The cumulative rule counts encode events: saveParticle is reached only when reserved0 != 0 (odd stages), so
    // even stages add no quantization event. cap_class = max(flat_class, events·E_class), with E_D = 4·1.5·q,
    // E_d = q, and 0 for every other class. A stage dump at stage = substep·2 + iteration uses
    // events = substep + iteration; tick2-hot and restored22-hot both use events = 16. The batch-3 sign rule stays
    // on the flat class cap, not the cumulative one.
    //
    // The integer grid has no flat word cap. Each 4-word node {momentum.x, momentum.y, mass, volume} is the sum
    // of per-contribution truncating fixed-point encodes, Σ_i trunc(Q·v_i) with Q = 1e7
    // (fork_conformance.slang:66-73, 77-94, 319-339), so it is neither an integer-transfer nor a control
    // semantic. The census `buffers.grid` section (first_stage 11, ±2, components_touched [0,1],
    // mass_volume_fields_contract_invariant) is superseded: the quadratic scatter weight is a function of the
    // particle position (types.slang:287-297), so w·mass and w·volume cannot be invariant once positions move.
    //
    // Per-node derivation. |trunc(a) - trunc(b)| <= |a - b| + 1 per contribution and a node sums N of them, so
    // the node allowance is N times (the per-contribution bound plus one). With
    // |Δ(w·X)| <= |Δw||X| + w|ΔX| + |Δw||ΔX|, |Δw| <= 0.75·(|Δpx| + |Δpy|) per node (the per-axis partials of
    // the quadratic weights are bounded by 1; 0.75 is the L1 coefficient of the 3×3 product weight under 2·δp),
    // and |ΔX| <= δd + 1.5·δD + Dmax·(|Δpx| + |Δpy|) for momentum. The momentum operand is carried by the
    // consumed particle state and bounded by its stage-input value plus one integration increment, which the same
    // δd/δD envelope covers. The cold-carried operands satisfy |Δmass| = |Δvolume| = 0, so mass and volume carry
    // only the |Δw|·m term. The second-order |Δw||ΔX| term is sub-quantum and absorbed by the +1.
    //
    // Acceptance consequence: the derived bound is ~27-44 quanta per contributor for momentum and 121 for
    // mass/volume against a realized 1-4, so the node accumulator is a coarse tripwire, not a fidelity
    // instrument. Fidelity is carried by the per-particle caps below and by the bit-exact control words.
    struct ForkWordBounds {
        double positionCap; // hot words 0-1
        double displacementCap; // hot words 2-3
        double deformationCap; // hot words 8-11
        double liquidDensityCap; // hot word 12
        bool gridExact; // fixture 0: node accumulator observed bit-exact; kept as a stricter-than-contract tripwire
        bool cumulativeDecodeEvents; // fixture 1: the gather classes accumulate one quantum per encode event
    };

    constexpr ForkWordBounds kForkWordBounds[] = {
        {8.0e-6, 5.0e-7, 1.0e-6, 1.0e-6, true, false}, // free-motion-complete-fields
        {8.0e-6, 5.0e-7, 1.0e-6, 1.0e-6, false, true}, // liquid-static-box
    };

    // ------------------------------------------------------------------
    // fixed-point grid encode and stage-input envelope
    // ------------------------------------------------------------------
    /// The shader encodes one contribution as trunc(Q·v) with Q = 1e7, so one fixed-point quantum is 1e-7.
    constexpr double kGridFixedPointScale = 1.0e7;
    /// One fixed-point quantum: the shader encodes contributions with Q = 1e7.
    constexpr double kForkDecodeQuantum = 1.0 / kGridFixedPointScale;
    /// D = 4·B and |offset| <= 1.5, so one decode-quantum perturbation of the gathered displacement moves a
    /// deformation entry by at most this much.
    constexpr double kDeformationGatherScale = 4.0;
    constexpr double kGatherOffsetMax = 1.5;
    constexpr double kDeformationEventEnvelope = kDeformationGatherScale * kGatherOffsetMax * kForkDecodeQuantum;
    /// Hot-word caps that bound how far a node's operands move between the observed and expected stages. They are
    /// read from the same table that grades the particle words, so the two cannot drift apart.
    constexpr double kGridPositionCap = kForkWordBounds[0].positionCap;
    constexpr double kGridDisplacementCap = kForkWordBounds[0].displacementCap;
    constexpr double kGridDeformationCap = kForkWordBounds[0].deformationCap;
    /// L1 bound on the quadratic weight derivative per axis; the derivation's 1.5 factor covers the D·offset term.
    constexpr double kGridWeightDerivative = 0.75;
    /// Shader support is floor(p)-1 .. floor(p)+1 per axis. Radius 2 absorbs a one-cell base shift under the
    /// position cap, and a node still counts a row at most once because the test is per row, not per cell.
    constexpr u32 kGridStencilRadius = 2u;

    struct ForkGridEnvelope {
        double m_max{}; // max enabled mass (cold word 0)
        double vol_max{}; // max enabled volume (cold word 1)
        double Xmax{}; // max (|dx| + |dy| + 1.5·||D||_1)
        double Dmax{}; // max ||D||_1
        std::size_t rows{}; // enabled row count
    };

    [[nodiscard]] ForkGridEnvelope gridEnvelope(
        const std::vector<u32> &stage_input_hot, const u32 *cold_words, const std::size_t row_count) {
        ForkGridEnvelope envelope;
        for (std::size_t row = 0; row < row_count; ++row) {
            const u32 *const hot = stage_input_hot.data() + row * 16u;
            if (hot[15u] == 0u) {
                continue;
            }

            const double dx = static_cast<double>(std::bit_cast<float>(hot[2u]));
            const double dy = static_cast<double>(std::bit_cast<float>(hot[3u]));
            const double deformationL1 = std::fabs(static_cast<double>(std::bit_cast<float>(hot[8u]))) +
                                         std::fabs(static_cast<double>(std::bit_cast<float>(hot[9u]))) +
                                         std::fabs(static_cast<double>(std::bit_cast<float>(hot[10u]))) +
                                         std::fabs(static_cast<double>(std::bit_cast<float>(hot[11u])));
            const double mass = static_cast<double>(std::bit_cast<float>(cold_words[row * 8u]));
            const double volume = static_cast<double>(std::bit_cast<float>(cold_words[row * 8u + 1u]));

            envelope.m_max = std::max(envelope.m_max, mass);
            envelope.vol_max = std::max(envelope.vol_max, volume);
            envelope.Xmax = std::max(envelope.Xmax, std::fabs(dx) + std::fabs(dy) + 1.5 * deformationL1);
            envelope.Dmax = std::max(envelope.Dmax, deformationL1);
            ++envelope.rows;
        }

        return envelope;
    }

    /// Per-contribution fixed-point allowance for one node component: the |Δw|·operand growth for the
    /// cold-carried mass/volume, and the full |Δw||X| + w|ΔX| growth for momentum, plus one quantum for the
    /// truncation gap of each contribution.
    [[nodiscard]] double gridComponentAllowance(const u32 component, const ForkGridEnvelope &envelope) noexcept {
        if (component == 2u or component == 3u) {
            const double operand_max = component == 2u ? envelope.m_max : envelope.vol_max;
            const double encoded_growth =
                kGridWeightDerivative * 2.0 * kGridPositionCap * kGridFixedPointScale * operand_max;

            return std::ceil(encoded_growth) + 1.0;
        }

        const double encoded_growth = kGridFixedPointScale *
                                      (kGridWeightDerivative * 2.0 * kGridPositionCap * envelope.Xmax +
                                          kGridWeightDerivative * (kGridDisplacementCap +
                                                                      1.5 * kGridDeformationCap +
                                                                      envelope.Dmax * 2.0 * kGridPositionCap));

        return std::ceil(encoded_growth) + 1.0;
    }

    /// Enabled rows whose widened stencil covers the node. The shader scatter support is
    /// floor(p)-1 .. floor(p)+1, so radius 2 never under-counts a contributor after a one-cell base shift.
    [[nodiscard]] std::size_t gridContributors(
        const std::vector<u32> &stage_input_hot, const std::size_t row_count, const u32 node_x,
        const u32 node_y) noexcept {
        std::size_t contributors = 0u;
        for (std::size_t row = 0; row < row_count; ++row) {
            const u32 *const hot = stage_input_hot.data() + row * 16u;
            if (hot[15u] == 0u) {
                continue;
            }

            const double px = static_cast<double>(std::bit_cast<float>(hot[0u]));
            const double py = static_cast<double>(std::bit_cast<float>(hot[1u]));
            const std::int64_t distance_x = std::abs(
                static_cast<std::int64_t>(node_x) - static_cast<std::int64_t>(std::floor(px)));
            const std::int64_t distance_y = std::abs(
                static_cast<std::int64_t>(node_y) - static_cast<std::int64_t>(std::floor(py)));
            if (distance_x <= static_cast<std::int64_t>(kGridStencilRadius) and
                distance_y <= static_cast<std::int64_t>(kGridStencilRadius)) {
                ++contributors;
            }
        }

        return contributors;
    }

    // ------------------------------------------------------------------
    // bounded and exact observations
    // ------------------------------------------------------------------
    void compareBoundedWords(
        const std::vector<u32> &actual, const u32 *expected, const ForkWordBounds &bounds, const char *case_name,
        u32 stage, const u32 events, const char *field) {
        for (std::size_t word = 0; word < actual.size(); ++word) {
            if (actual[word] == expected[word]) {
                continue;
            }

            const u32 position = static_cast<u32>(word % 16u);
            const bool negative = ((actual[word] ^ expected[word]) & 0x80000000u) != 0u;
            const float expected_f = std::bit_cast<float>(expected[word]);
            const float actual_f = std::bit_cast<float>(actual[word]);
            const double delta = static_cast<double>(actual_f) - static_cast<double>(expected_f);

            const float ulp = std::nextafter(std::fabs(expected_f), std::numeric_limits<float>::infinity()) -
                              std::fabs(expected_f);
            const double flat_cap = position == 0u or position == 1u     ? bounds.positionCap
                                    : position == 2u or position == 3u   ? bounds.displacementCap
                                    : position >= 8u and position <= 11u ? bounds.deformationCap
                                    : position == 12u                    ? bounds.liquidDensityCap
                                                                         : 0.0; // words 4-7 and 13-15 stay bit-exact

            const double per_event = position >= 8u and position <= 11u ? kDeformationEventEnvelope
                                     : position == 2u or position == 3u ? kForkDecodeQuantum
                                                                        : 0.0;
            const double cap = bounds.cumulativeDecodeEvents and per_event > 0.0
                                   ? std::max(flat_cap, static_cast<double>(events) * per_event)
                                   : flat_cap;
            const double limit = std::max(cap, 8.0 * static_cast<double>(ulp));

            // Batch 3: below the class cap, a near-cancelled residual's sign is contract/driver latitude
            // (fix-2 word 73 evidence); the magnitude bound is the discriminator there.
            const bool sign_flip = negative and std::min(
                                                    std::fabs(static_cast<double>(expected_f)),
                                                    std::fabs(static_cast<double>(actual_f))) > flat_cap;

            std::fprintf(
                stderr,
                "fork bounded-divergence case=%s stage=%u buffer=%s word=%zu "
                "actual=%08x expected=%08x delta=%.17e events=%u flat_cap=%.17e cap=%.17e%s\n",
                case_name, stage, field, word, actual[word], expected[word], delta, events, flat_cap, limit,
                negative ? "; sign-flip" : "");
            PPR_TEST_ASSERT(not sign_flip and flat_cap != 0.0 and std::fabs(delta) <= limit);
        }
    }

    void compareWords(
        const std::vector<u32> &actual, const u32 *expected, const char *case_name, u32 stage, const char *field) {
        for (std::size_t word = 0; word < actual.size(); ++word) {
            if (actual[word] != expected[word]) {
                std::fprintf(
                    stderr,
                    "fork divergence case=%s stage=%u buffer=%s word=%zu actual=%08x expected=%08x; exact-bound=0\n",
                    case_name, stage, field, word, actual[word], expected[word]);
                PPR_TEST_ASSERT(actual[word] == expected[word]);
            }
        }
    }

    void compareIntegerGrid(
        const std::vector<u32> &actual, const u32 *expected, const std::vector<u32> &stage_input_hot,
        const u32 *cold_words, const std::vector<u32> &budget_words, const ForkWordBounds &bounds,
        const std::size_t row_count, const char *case_name, const u32 stage, const u32 grid_width) {
        if (bounds.gridExact) {
            compareWords(actual, expected, case_name, stage, "integer-grid");
            return;
        }

        PPR_TEST_ASSERT(stage_input_hot.size() == row_count * 16u);
        PPR_TEST_ASSERT(budget_words.size() == actual.size());

        const ForkGridEnvelope envelope = gridEnvelope(stage_input_hot, cold_words, row_count);
        std::size_t differing = 0u;
        for (std::size_t word = 0; word < actual.size(); ++word) {
            if (actual[word] == expected[word]) {
                continue;
            }

            const std::size_t node = word / 4u;
            const u32 component = static_cast<u32>(word % 4u);
            const u32 node_x = static_cast<u32>(node % grid_width);
            const u32 node_y = static_cast<u32>(node / grid_width);

            const std::int64_t delta = static_cast<std::int64_t>(static_cast<std::int32_t>(actual[word])) -
                                       static_cast<std::int64_t>(static_cast<std::int32_t>(expected[word]));
            const std::size_t contributors = gridContributors(stage_input_hot, row_count, node_x, node_y);
            const double allowance = gridComponentAllowance(component, envelope);
            const double bound = static_cast<double>(contributors) * allowance;
            ++differing;

            std::fprintf(
                stderr,
                "fork bounded-divergence case=%s stage=%u buffer=integer-grid word=%zu node_x=%u node_y=%u "
                "component=%u actual=%08x expected=%08x delta=%lld actual_value=%.9e expected_value=%.9e "
                "contributors=%zu allowance=%.17e bound=%.17e budget=%08x rows=%zu\n",
                case_name, stage, word, node_x, node_y, component, actual[word], expected[word],
                static_cast<long long>(delta),
                static_cast<double>(static_cast<std::int32_t>(actual[word])) / kGridFixedPointScale,
                static_cast<double>(static_cast<std::int32_t>(expected[word])) / kGridFixedPointScale, contributors,
                allowance, bound, budget_words[word], envelope.rows);
            PPR_TEST_ASSERT(std::fabs(static_cast<double>(delta)) <= bound and bound > 0.0);
        }

        PPR_TEST_ASSERT(differing <= 36u * row_count);
    }

    // ------------------------------------------------------------------
    // pinned actual-WGSL stage/tick and guard contracts
    // ------------------------------------------------------------------
    PPR_UNIT_TEST (actual_wgsl_stages_and_exact_semantic_restart) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        for (u32 caseIndex = 0; caseIndex < u32(std::size(oracleCases)); ++caseIndex) {
            const auto &input = oracleCases[caseIndex];
            State native{fixture, input};
            native.bucketize();
            PPR_TEST_ASSERT(native.read(native.errors)[0u] == 0u);
            native.checkBucketIdentities(input);

            std::vector<u32> first_hot, first_cold;
            for (u32 tick = 0; tick < 2; ++tick) {
                if (tick != 0u) {
                    native.resetGridFrontier();
                    native.bucketize();
                }

                for (u32 substep = 0; substep < 8; ++substep) {
                    for (u32 iteration = 0; iteration < 2; ++iteration) {
                        // The grid envelope is derived from the state this stage consumes, so it must be read
                        // before the step advances the particle words.
                        std::vector<u32> stage_input_hot;
                        if (tick == 0u) {
                            stage_input_hot = native.read(native.hot);
                        }

                        native.step(iteration);
                        PPR_TEST_ASSERT(native.read(native.errors)[0u] == 0u);
                        if (tick == 0u) {
                            const auto &expected = input.stages[substep * 2u + iteration];
                            compareBoundedWords(
                                native.read(native.hot), expected.hot, kForkWordBounds[caseIndex], input.name,
                                substep * 2u + iteration, substep + iteration, "hot64");
                            compareWords(
                                native.read(native.cold), expected.cold, input.name, substep * 2u + iteration,
                                "cold32");
                            compareIntegerGrid(
                                native.read(native.grid[native.grid_index]), expected.grid, stage_input_hot, input.cold,
                                native.read(native.budget), kForkWordBounds[caseIndex], input.count, input.name,
                                substep * 2u + iteration, native.parameters.m_grid_width);
                        }
                    }

                    native.bucketize();
                    PPR_TEST_ASSERT(native.read(native.errors)[0u] == 0u);
                    native.checkBucketIdentities(input);
                }

                if (tick == 0u) {
                    first_hot = native.read(native.hot);
                    first_cold = native.read(native.cold);
                }
            }

            compareBoundedWords(
                native.read(native.hot), input.secondHot, kForkWordBounds[caseIndex], input.name, 16u, 16u,
                "tick2-hot");
            compareWords(native.read(native.cold), input.secondCold, input.name, 16u, "tick2-cold");

            const auto native_second_hot = native.read(native.hot);
            const auto native_second_cold = native.read(native.cold);

            State restored{fixture, input, first_hot.data(), first_cold.data()};
            restored.bucketize();
            restored.checkBucketIdentities(input);

            for (u32 substep = 0; substep < 8; ++substep) {
                restored.step(0u);
                restored.step(1u);
                restored.bucketize();
                PPR_TEST_ASSERT(restored.read(restored.errors)[0u] == 0u);
            }

            compareBoundedWords(
                restored.read(restored.hot), input.secondHot, kForkWordBounds[caseIndex], input.name, 17u, 16u,
                "restored22-hot");
            compareWords(restored.read(restored.cold), input.secondCold, input.name, 17u, "restored22-cold");

            compareWords(
                restored.read(restored.hot), native_second_hot.data(), input.name, 17u, "restored22-vs-native-tick2");
            compareWords(
                restored.read(restored.cold), native_second_cold.data(), input.name, 17u,
                "restored22-vs-native-tick2-cold");
        }
    };

    // ------------------------------------------------------------------
    // production K2 path against the independent frozen WGSL journal
    // ------------------------------------------------------------------
    std::vector<u32> projectWords(const u32 *words, const std::vector<u32> &original_rows, const u32 stride) {
        std::vector<u32> result;
        result.reserve(original_rows.size() * stride);
        for (const u32 row: original_rows) {
            result.insert(result.end(), words + row * stride, words + (row + 1u) * stride);
        }

        return result;
    }

    matter::GpuSnapshot captureProduction(matter::GpuSolver &solver) {
        PPR_TEST_ASSERT(not solver.beginSnapshot(solver.accounting().m_epoch));
        PPR_TEST_ASSERT(not solver.drain());
        auto snapshot = solver.pollSnapshot();
        PPR_TEST_ASSERT(snapshot and snapshot->has_value());
        return std::move(**snapshot);
    }

    void compareProduction(
        const matter::GpuSnapshot &snapshot, const u32 *expected_hot, const u32 *expected_cold,
        const std::vector<u32> &original_rows, const ForkWordBounds &bounds, const char *name, const u32 stage,
        const u32 events) {
        PPR_TEST_ASSERT(snapshot.m_particles.size() == original_rows.size());
        std::vector<u32> hot, cold;
        for (const auto &particle: snapshot.m_particles) {
            const auto h = std::bit_cast<std::array<u32, 16u>>(matter::packWrite(particle));
            const auto compact = std::bit_cast<std::array<u32, 6u>>(matter::packReadonly(particle));
            const auto c = oracleCold(compact.data());
            hot.insert(hot.end(), h.begin(), h.end());
            cold.insert(cold.end(), c.begin(), c.end());
        }

        const auto projected_hot = projectWords(expected_hot, original_rows, 16u);
        const auto projected_cold = projectWords(expected_cold, original_rows, 8u);
        compareBoundedWords(hot, projected_hot.data(), bounds, name, stage, events, "production-hot64");
        compareWords(cold, projected_cold.data(), name, stage, "production-cold32");
    }

    PPR_UNIT_TEST (production_k2_stages_tick2_and_restored22_match_actual_wgsl) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        for (u32 case_index = 0u; case_index < std::size(oracleCases); ++case_index) {
            const auto &input = oracleCases[case_index];
            const auto parameters = std::bit_cast<GpuParameters>(input.parameters);
            Array<matter::GpuParticle> particles;
            std::vector<u32> original_rows;
            for (u32 original = 0u; original < input.count; ++original) {
                std::array<u32, 16u> hot_words{};
                std::array<u32, 8u> cold_words{};
                std::copy_n(input.hot + original * 16u, 16u, hot_words.begin());
                std::copy_n(input.cold + original * 8u, 8u, cold_words.begin());
                const auto hot = std::bit_cast<matter::ParticleWrite>(hot_words);
                const auto cold = std::bit_cast<matter::ParticleReadonly>(compactCold(cold_words.data()));
                if (hot.m_enabled == 0.0f) {
                    // The fork's bucket admission skips exactly these rows, so
                    // they produce no count/scatter. Full disabled-row behavior
                    // remains covered by the unprojected conformance fixture.
                    PPR_TEST_ASSERT(hot_words[15u] == 0u);
                    continue;
                }

                PPR_TEST_ASSERT(hot.m_enabled == 1.0f);
                original_rows.push_back(original);
                particles.push_back(matter::unpack(hot, cold));
            }

            matter::GpuSolver solver;
            matter::GpuSolver restored;
            PPR_DEFER {
                PPR_TEST_ASSERT(not restored.shutdown());
                PPR_TEST_ASSERT(not solver.shutdown());
            };
            PPR_TEST_ASSERT(not solver.initialize(
                fixture.rhi_service->getDevice(), *fixture.queue, *fixture.shaders, std::filesystem::current_path(),
                parameters));
            const std::span<const GpuStaticShape> shapes{&input.shape, parameters.m_shape_count};
            PPR_TEST_ASSERT(not solver.prepareReplacement(particles, shapes, solver.accounting().m_epoch));
            solver.publishReplacement();

            matter::GpuSnapshot first_tick;
            for (u32 substep = 0u; substep < 8u; ++substep) {
                PPR_TEST_ASSERT(not solver.submitResidentTick(solver.accounting().m_epoch, 1u, 2u));
                PPR_TEST_ASSERT(not solver.drain());
                const auto ready = solver.pollTick();
                PPR_TEST_ASSERT(ready and *ready);
                solver.publishTick();

                first_tick = captureProduction(solver);
                const u32 stage = substep * 2u + 1u;
                compareProduction(
                    first_tick, input.stages[stage].hot, input.stages[stage].cold, original_rows,
                    kForkWordBounds[case_index], input.name, stage, substep + 1u);
            }

            PPR_TEST_ASSERT(not solver.submitResidentTick(solver.accounting().m_epoch, 8u, 2u));
            PPR_TEST_ASSERT(not solver.drain());
            const auto complete = solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            solver.publishTick();
            const auto second_tick = captureProduction(solver);
            compareProduction(
                second_tick, input.secondHot, input.secondCold, original_rows, kForkWordBounds[case_index], input.name,
                16u, 16u);

            PPR_TEST_ASSERT(not restored.initialize(
                fixture.rhi_service->getDevice(), *fixture.queue, *fixture.shaders, std::filesystem::current_path(),
                parameters));
            PPR_TEST_ASSERT(
                not restored.prepareReplacement(first_tick.m_particles, shapes, restored.accounting().m_epoch));
            restored.publishReplacement();
            PPR_TEST_ASSERT(not restored.submitResidentTick(restored.accounting().m_epoch, 8u, 2u));
            PPR_TEST_ASSERT(not restored.drain());
            const auto restored_ready = restored.pollTick();
            PPR_TEST_ASSERT(restored_ready and *restored_ready);
            restored.publishTick();
            const auto restored_tick = captureProduction(restored);
            compareProduction(
                restored_tick, input.secondHot, input.secondCold, original_rows, kForkWordBounds[case_index],
                input.name, 17u, 16u);

            PPR_TEST_ASSERT(restored_tick.m_particles.size() == second_tick.m_particles.size());
            PPR_TEST_ASSERT(
                std::memcmp(
                    restored_tick.m_particles.data(), second_tick.m_particles.data(),
                    second_tick.m_particles.size() * sizeof(matter::GpuParticle)) == 0);
        }
    };

    PPR_UNIT_TEST (guards_reject_before_shared_signed_accumulation) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto &input = oracleCases[0];
        for (u32 probe = 0; probe < 3; ++probe) {
            std::vector<u32> hot(32u * 16u), cold(32u * 8u);
            for (u32 row = 0; row < 32; ++row) {
                std::copy_n(input.hot, 16u, hot.data() + row * 16u);
                std::copy_n(input.cold, 8u, cold.data() + row * 8u);
                hot[row * 16u + 2u] = std::bit_cast<u32>(
                    probe == 0u   ? std::numeric_limits<float>::quiet_NaN()
                    : probe == 1u ? 1.0e30f
                                  : 100.0f);
            }

            State native{fixture, input, hot.data(), cold.data(), 32u};
            native.bucketize();
            native.step(0u);

            const u32 expected = probe == 0u ? 8u : probe == 1u ? 2u : 4u;
            PPR_TEST_ASSERT((native.read(native.errors)[0u] & expected) != 0u);
            PPR_TEST_ASSERT(native.read(native.hot) == hot);
        }
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // dedicated fork conformance registration
    // ------------------------------------------------------------------
    const UnitTest &matter_forkTests() noexcept {
        static const UnitTest group = UnitTest::Named("matter_fork") / [](UnitTest::IRun &run) {
            run.recurse(
                {detail::MatterFork::actual_wgsl_stages_and_exact_semantic_restart,
                    detail::MatterFork::guards_reject_before_shared_signed_accumulation,
                    detail::MatterFork::production_k2_stages_tick2_and_restored22_match_actual_wgsl});
        };

        return group;
    }
}
