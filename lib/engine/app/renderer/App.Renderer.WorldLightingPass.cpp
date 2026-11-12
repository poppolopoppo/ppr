module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>

module engine.app;

import :renderer.frame_fence;
import :renderer.types;
import :renderer.world_lighting;
import :renderer.world_lighting_pass;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(WorldLightingPass, info, none)

    namespace {
        // ------------------------------------------------------------------
        // projection buffer and status plumbing
        // ------------------------------------------------------------------

        rhi::BufferRange projectionRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code projectionStorageBuffer(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial,
            rhi::ComPtr<rhi::IBuffer> &result, const char *const label) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = stride;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource) |
                static_cast<u32>(rhi::BufferUsage::CopyDestination));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            desc.label = label;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, device.createBuffer(desc, initial, result.writeRef()));
            return {};
        }

        std::error_code projectionReadbackBuffer(
            rhi::IDevice &device, const u64 bytes, rhi::ComPtr<rhi::IBuffer> &result, const char *const label) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = 0u;
            desc.memoryType = rhi::MemoryType::ReadBack;
            desc.usage = rhi::BufferUsage::CopyDestination;
            desc.defaultState = rhi::ResourceState::CopyDestination;
            desc.label = label;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, device.createBuffer(desc, nullptr, result.writeRef()));
            return {};
        }

        constexpr std::array<const char *, WorldLightingPass::kNumericDispatchCount> kProjectionEntries{
            "clearWinners", "reduceKeys", "reduceRows", "resolveCells"};

        constexpr std::array<const char *, kNumericStatusRecords> kNumericStageNames{
            "clear_winners", "reduce_keys", "reduce_rows", "resolve_cells", "prepare_source", "update_terminal",
            "update_cascade", "reconstruct_fine"};

        constexpr std::array<const char *, WorldLightingPass::kTransportEntryCount> kTransportEntries{
            "buildVisibility", "buildJump"};

        // Entry slots are fixed: the source module owns record 0, the cascade
        // module owns records 1 and 2, and the fine module owns record 7.
        constexpr std::array<const char *, 1u> kSourceEntries{"prepareSource"};
        constexpr std::array<const char *, 2u> kCascadeEntries{"updateTerminal", "updateCascade"};
        constexpr std::array<const char *, 1u> kFineEntries{"reconstructFine"};

        constexpr std::array<const char *, 2u> kInspectionEntries{"vertexInspect", "fragmentInspect"};

        // Mirrored field-for-field by assets/shaders/world_lighting_inspect.slang.
        struct alignas(16) InspectionStageFrame {
            float4x4 m_inverse_view_projection{identity_v};
            float4 m_viewport{};
            u32 m_diagnostic{};
            u32 m_cascade_level{1u};
            u32 m_encode_srgb{};
            float m_cells_per_metre{1.0f};
        };

        static_assert(sizeof(InspectionStageFrame) == 96u);
        static_assert(alignof(InspectionStageFrame) == 16u);
        static_assert(PPR_OFFSETOF(InspectionStageFrame, m_cells_per_metre) == 92u);
        static_assert(PPR_OFFSETOF(InspectionStageFrame, m_inverse_view_projection) == 0u);
        static_assert(PPR_OFFSETOF(InspectionStageFrame, m_viewport) == 64u);
        static_assert(PPR_OFFSETOF(InspectionStageFrame, m_diagnostic) == 80u);

        std::error_code validateProjectionInputs(const lighting::WorldLightingInputs &inputs) {
            // Only the resources these dispatches bind are required. The source
            // palette and thermal table belong to the later source/transport
            // stage and stay in `WorldLightingInputs` for that owner.
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, inputs.m_terrain and inputs.m_visible_materials
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_connected));

            rhi::IBuffer *const terrain = inputs.m_terrain.get();
            rhi::IBuffer *const visuals = inputs.m_visible_materials.get();

            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, terrain->getDesc().size == WorldLightingPass::kResolvedCellBytes and
                                           terrain->getDesc().elementSize == sizeof(u32)
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::invalid_argument));

            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, inputs.m_material_count > 0u and
                                           inputs.m_material_count <= 65536u and
                                           visuals->getDesc().elementSize == 96u and
                                           visuals->getDesc().size >= static_cast<u64>(inputs.m_material_count) * 96u
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::invalid_argument));

            const auto &particles = inputs.m_particles;
            const bool published = particles.m_numerical or particles.m_readonly or particles.m_metadata;
            if (not published) {
                return {};
            }

            // A stale committed view is not an empty world: never fabricate a
            // zero-candidate publication from a revoked epoch.
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, particles.current() and
                                           particles.m_metadata->m_rows and
                                           particles.m_metadata->m_count <= kGridParticleCapacity and
                                           particles.m_metadata->m_moving_count <= particles.m_metadata->m_count and
                                           std::isfinite(particles.m_origin) and
                                           particles.m_origin >= 0.0
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::resource_unavailable_try_again));
            return {};
        }

        // The cached-source stage indexes the derived eight-byte palette up to
        // material_count-1 and the committed thermal table by row. Both are
        // required publication inputs with exact shapes; a missing, undersized
        // or differently-strided buffer is rejected before any encoding.
        std::error_code validateSourceInputs(const lighting::WorldLightingInputs &inputs) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, inputs.m_source_materials and inputs.m_thermal_lut
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_connected));

            rhi::IBuffer *const sources = inputs.m_source_materials.get();
            rhi::IBuffer *const thermal = inputs.m_thermal_lut.get();

            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, sources->getDesc().elementSize == sizeof(lighting::SourceMaterial) and
                                           sources->getDesc().size >= static_cast<u64>(inputs.m_material_count) *
                                                                          sizeof(lighting::SourceMaterial)
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, thermal->getDesc().size == lighting::kThermalLutBytes and
                                           thermal->getDesc().elementSize == lighting::kThermalLutStride
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::invalid_argument));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // initialization and teardown
    // ------------------------------------------------------------------

    std::error_code validateLightingInspectionSettings(const LightingInspectionSettings &settings) noexcept {
        if (lighting_diagnostic_domain.nameOf(settings.m_diagnostic).empty()) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        if (settings.m_cascade < WorldLightingPass::kInspectionCascadeMin or
            settings.m_cascade > WorldLightingPass::kInspectionCascadeMax) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        return {};
    }

    WorldLightingPass::~WorldLightingPass() {
        releaseResources_();
    }

    std::error_code WorldLightingPass::initialize(IShaderService &shaders, const std::filesystem::path &root) {
        m_shaders = safe_ptr{&shaders};
        m_root = root;
        if (m_ready) {
            return {};
        }

        const safe_ptr<IRhiService> rhi_service = IRhiService::get();
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            rhi_service != nullptr ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        rhi::IDevice &device = rhi_service->getDevice();
        m_device = &device;

        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            shaders.loadModuleFromFile(
                root / "shaders" / "world_lighting_projection.slang", "world_lighting_projection", module.writeRef()));

        std::array<ProjectionProgram, kNumericDispatchCount> programs{};
        for (u32 entry = 0u; entry < kNumericDispatchCount; ++entry) {
            shader::ComPtr<slang::IEntryPoint> point{};
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, module->findEntryPointByName(kProjectionEntries[entry], point.writeRef()));

            slang::IComponentType *entry_points[]{point.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module.get();
            description.slangEntryPoints = entry_points;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                device.createShaderProgram(description, programs[entry].m_program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = programs[entry].m_program;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, device.createComputePipeline(pipeline, programs[entry].m_pipeline.writeRef()));
        }

        if (not m_resources) {
            m_resources = std::make_unique<ProjectionResources>();
        }

        for (u32 entry = 0u; entry < kNumericDispatchCount; ++entry) {
            m_resources->m_programs[entry] = std::move(programs[entry]);
        }

        // Full-domain acceleration entries build both visibility levels and the
        // per-cell jump table from the resolved cells and authored palette.
        shader::SharedModule transport_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, shaders.loadModuleFromFile(
                                   root / "shaders" / "world_lighting_visibility.slang", "world_lighting_visibility",
                                   transport_module.writeRef()));

        std::array<ProjectionProgram, kTransportEntryCount> transport_programs{};
        for (u32 entry = 0u; entry < kTransportEntryCount; ++entry) {
            shader::ComPtr<slang::IEntryPoint> point{};
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, transport_module->findEntryPointByName(kTransportEntries[entry], point.writeRef()));

            slang::IComponentType *entry_points[]{point.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = transport_module.get();
            description.slangEntryPoints = entry_points;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                device.createShaderProgram(
                    description, transport_programs[entry].m_program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = transport_programs[entry].m_program;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                device.createComputePipeline(pipeline, transport_programs[entry].m_pipeline.writeRef()));
        }

        for (u32 entry = 0u; entry < kTransportEntryCount; ++entry) {
            m_resources->m_transport_programs[entry] = std::move(transport_programs[entry]);
        }

        // Cached-source, cascade and fine-reconstruction entries. One 40 MiB
        // interval store carries L1..L5; the sky, schedule, CH basis and codecs
        // are include-only fragments.
        const auto load_world_lighting_programs = [&](const std::filesystem::path &file, const char *const label,
                                                      const u32 first_entry, const std::span<const char *const> entries,
                                                      ProjectionProgram *const programs) {
            shader::SharedModule module{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, shaders.loadModuleFromFile(file, label, module.writeRef()));

            for (u32 offset = 0u; offset < entries.size(); ++offset) {
                const u32 entry = first_entry + offset;
                shader::ComPtr<slang::IEntryPoint> point{};
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, module->findEntryPointByName(entries[offset], point.writeRef()));

                slang::IComponentType *entry_points[]{point.get()};
                rhi::ShaderProgramDesc description{};
                description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                description.slangGlobalScope = module.get();
                description.slangEntryPoints = entry_points;
                description.slangEntryPointCount = 1u;
                shader::Diagnose diagnostics;
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, device.createShaderProgram(
                                           description, programs[entry].m_program.writeRef(), diagnostics.writeRef()));

                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = programs[entry].m_program;
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, device.createComputePipeline(pipeline, programs[entry].m_pipeline.writeRef()));
            }

            return std::error_code{};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, load_world_lighting_programs(
                                   root / "shaders" / "world_lighting_source.slang", "world_lighting_source", 0u,
                                   kSourceEntries, m_resources->m_cascade_programs.data()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, load_world_lighting_programs(
                                   root / "shaders" / "world_lighting_cascade.slang", "world_lighting_cascade", 1u,
                                   kCascadeEntries, m_resources->m_cascade_programs.data()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, load_world_lighting_programs(
                                   root / "shaders" / "world_lighting_fine.slang", "world_lighting_fine", 0u,
                                   kFineEntries, m_resources->m_fine_programs.data()));

        // Retained inspection: one fullscreen render program. Its pipeline is
        // created per acquired-surface signature on first C-stage use.
        shader::SharedModule inspect_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, shaders.loadModuleFromFile(
                                   root / "shaders" / "world_lighting_inspect.slang", "world_lighting_inspect",
                                   inspect_module.writeRef()));

        std::array<shader::ComPtr<slang::IEntryPoint>, kInspectionEntries.size()> inspect_points{};
        for (u32 entry = 0u; entry < kInspectionEntries.size(); ++entry) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                inspect_module->findEntryPointByName(kInspectionEntries[entry], inspect_points[entry].writeRef()));
        }

        slang::IComponentType *inspection_entries[]{
            inspect_points[0u].get(),
            inspect_points[1u].get(),
        };
        rhi::ShaderProgramDesc inspection_description{};
        inspection_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        inspection_description.slangGlobalScope = inspect_module.get();
        inspection_description.slangEntryPoints = inspection_entries;
        inspection_description.slangEntryPointCount = static_cast<u32>(std::size(inspection_entries));
        shader::Diagnose inspection_diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, device.createShaderProgram(
                                   inspection_description, m_resources->m_inspection.m_program.writeRef(),
                                   inspection_diagnostics.writeRef()));

        m_ready = true;
        PPR_LOG(WorldLightingPass, info, "WorldLightingPass initialized");
        return {};
    }

    std::error_code WorldLightingPass::ensureResources_(rhi::IDevice &device) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources and (m_device == nullptr or m_device == &device)
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::state_not_recoverable));

        m_device = &device;
        auto &resources = *m_resources;
        if (resources.m_resolved) {
            return ensureTransportResources_(device);
        }

        const std::array<NumericStatusRecord, kNumericStatusRecords> initial_status{};
        for (u32 index = 0u; index < 2u; ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                projectionStorageBuffer(
                    device, kScratchGridBytes, sizeof(u32), nullptr, resources.m_scratch[index],
                    index == 0u ? "world-lighting best-key scratch" : "world-lighting best-row scratch"));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, projectionStorageBuffer(
                                   device, kResolvedCellBytes, sizeof(u32), nullptr, resources.m_resolved,
                                   "world-lighting resolved cells"));

        const GridParticleMetadataRow empty_metadata{};
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, projectionStorageBuffer(
                                   device, sizeof(GridParticleMetadataRow), sizeof(GridParticleMetadataRow),
                                   &empty_metadata, resources.m_empty_metadata, "world-lighting empty metadata row"));

        for (u32 slot = 0u; slot < kStatusRingSize; ++slot) {
            auto &attempt = resources.m_status[slot];
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, projectionStorageBuffer(
                                       device, kStatusAttemptBytes, sizeof(u32), initial_status.data(),
                                       attempt.m_status, "world-lighting numeric status"));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                projectionReadbackBuffer(
                    device, kStatusAttemptBytes, attempt.m_readback, "world-lighting numeric status readback"));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                device.mapBuffer(attempt.m_readback, rhi::CpuAccessMode::Read, &attempt.m_readback_mapped));

            for (u32 entry = 0u; entry < kNumericDispatchCount; ++entry) {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass,
                    device.createRootShaderObject(
                        resources.m_programs[entry].m_program, resources.m_roots[entry][slot].writeRef()));
            }

            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                device.createRootShaderObject(
                    resources.m_inspection.m_program, resources.m_inspection_roots[slot].writeRef()));
        }

        return ensureTransportResources_(device);
    }

    void WorldLightingPass::releaseResources_() noexcept {
        if (not m_resources) {
            return;
        }

        // Best-effort teardown: a failed unmap only means the CPU view is
        // released with the buffer.
        for (auto &attempt: m_resources->m_status) {
            if (m_device != nullptr and attempt.m_readback and attempt.m_readback_mapped != nullptr) {
                std::ignore = m_device->unmapBuffer(attempt.m_readback);
            }
            attempt.m_readback_mapped = nullptr;
        }

        std::ignore = m_inspection_releases.drainAll();
        m_resources.reset();
        m_ready = false;
        m_device = nullptr;
    }

    std::error_code WorldLightingPass::shutdown() {
        std::error_code first_err{};
        if (m_resources) {
            for (u32 slot = 0u; slot < kStatusRingSize; ++slot) {
                auto &attempt = m_resources->m_status[slot];
                if (not attempt.m_pending) {
                    continue;
                }

                if (attempt.m_submission == kFenceValueNone) {
                    // Definitely unsubmitted: its slot is released normally.
                    clearAttempt_(attempt);
                    continue;
                }

                // The owner proved GPU idle before pass teardown, so the small
                // completion-fenced readback can be inspected here.
                PPR_RETAIN_ERROR_ON_FAIL(WorldLightingPass, first_err, inspectAttempt_(attempt));
            }
        }

        releaseResources_();
        m_shaders = nullptr;
        m_root.clear();
        PPR_LOG(WorldLightingPass, info, "WorldLightingPass shut down");
        return first_err;
    }

    void WorldLightingPass::notifyDeviceLost() noexcept {
        // The GPU objects are gone: no completion can be inspected.
        releaseResources_();
    }

    // ------------------------------------------------------------------
    // attempt preparation and status copy
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::prepare(
        const PreRenderContext &context, const lighting::WorldLightingInputs &inputs, const WorldLightingFrame &frame) {
        if (not m_ready) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, m_shaders.isValid() and not m_root.empty()
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_connected));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, initialize(*m_shaders, m_root));
        }

        // A prepared attempt that was neither submitted nor discarded still owns
        // its status slot, constants and recorded root bindings. A second
        // prepare would silently re-associate the next submission fence with
        // the wrong slot and leave the first attempt's GPU work releasable as
        // "unsubmitted". Reject the call before it touches anything; the caller
        // settles the armed attempt with notifySubmitted or discardUnsubmitted
        // first.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources and m_resources->m_armed
                                   ? std::make_error_code(std::errc::operation_not_permitted)
                                   : std::error_code{});

        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, validateProjectionInputs(inputs));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, validateSourceInputs(inputs));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, frame.m_sky_hour >= -1 and frame.m_sky_hour <= 23
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, ensureResources_(context.m_device));

        // The injected active presentation time must not move backwards. The
        // accepted submission commits the new baseline; a discarded attempt
        // leaves it untouched so its elapsed active time belongs to the next
        // accepted lighting update.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, frame.m_active_elapsed >= m_resources->m_accepted_active_elapsed
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::invalid_argument));

        // A world whose accepted-frame counter is exhausted cannot derive a
        // current or retained probe frame. Reject admission before any slot,
        // encoding or storage is touched rather than wrapping the counter.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources->m_accepted_attempts != std::numeric_limits<u64>::max()
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::result_out_of_range));

        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, selectStatusSlot_(context.m_completed_fence_value));

        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];
        attempt.m_inputs = inputs;
        resources.m_bound_source_materials = inputs.m_source_materials;
        resources.m_bound_thermal_lut = inputs.m_thermal_lut;
        attempt.m_active_elapsed = frame.m_active_elapsed;
        resources.m_world = inputs.m_particles.current() ? inputs.m_particles.m_world : GridWorldIdentity{};
        resources.m_last_sky_hour = frame.m_sky_hour;

        if (auto encoding = encodeProjection_(context); encoding) {
            // Encoding failed before any submission: release the armed slot so
            // a frame that never reached the GPU leaves no pending ticket.
            discardUnsubmitted();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, encoding);
        }

        // The resolved cells are now ordered before the visibility read, and the
        // visibility write is ordered before the jump read and the later trace
        // readers inside the same encoder.
        if (auto encoding = encodeAcceleration_(context); encoding) {
            discardUnsubmitted();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, encoding);
        }

        // Source preparation, then L5 through L1 with explicit dependencies
        // between the stages and before any tracing reader. The source cache and
        // intervals are shared in place, never reallocated per frame.
        if (auto encoding = encodeCascades_(context, frame); encoding) {
            discardUnsubmitted();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, encoding);
        }

        // Fine reconstruction runs after every source-preparation CH read and
        // before scene readers. It reads only its own prior record and writes
        // that record in place.
        if (auto encoding = encodeFine_(context); encoding) {
            discardUnsubmitted();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, encoding);
        }

        return {};
    }

    std::error_code WorldLightingPass::finalize(const PreRenderContext &context) {
        if (not m_resources or not m_resources->m_armed) {
            return {};
        }

        auto &attempt = m_resources->m_status[m_resources->m_attempt_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, attempt.m_status and attempt.m_readback
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::not_connected));

        context.m_encoder.pushDebugGroup("WorldLighting.Status", {});
        PPR_DEFER {
            context.m_encoder.popDebugGroup();
        };

        context.m_encoder.setBufferState(attempt.m_status, rhi::ResourceState::CopySource);
        context.m_encoder.copyBuffer(attempt.m_readback, 0u, attempt.m_status, 0u, kStatusAttemptBytes);
        context.m_encoder.setBufferState(attempt.m_status, rhi::ResourceState::UnorderedAccess);
        attempt.m_copy_encoded = true;
        return {};
    }

    // ------------------------------------------------------------------
    // status slots and settlement
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::selectStatusSlot_(const FenceCompletionValue completed) noexcept {
        auto &resources = *m_resources;
        for (u32 offset = 0u; offset < kStatusRingSize; ++offset) {
            const u32 slot = (resources.m_status_cursor + offset) % kStatusRingSize;
            auto &attempt = resources.m_status[slot];
            if (attempt.m_pending) {
                continue;
            }

            if (not slotFenceComplete(completed, attempt.m_submission)) {
                continue;
            }

            resources.m_attempt_slot = slot;
            resources.m_status_cursor = (slot + 1u) % kStatusRingSize;
            resources.m_armed = true;
            ++resources.m_attempt_sequence;
            attempt.m_pending = true;
            attempt.m_copy_encoded = false;
            attempt.m_submission = kFenceValueNone;
            attempt.m_read_submission = kFenceValueNone;
            attempt.m_accepted_frame = 0u;
            return {};
        }

        // Ordinary non-admission: never overwrite an uncompleted or
        // uninspected status slot.
        return std::make_error_code(std::errc::device_or_resource_busy);
    }

    void WorldLightingPass::clearAttempt_(ProjectionStatusSlot &slot) noexcept {
        slot.m_inputs = {};
        slot.m_submission = kFenceValueNone;
        slot.m_read_submission = kFenceValueNone;
        slot.m_active_elapsed = {};
        slot.m_world_generation = 0u;
        slot.m_accepted_frame = 0u;
        slot.m_pending = false;
        slot.m_copy_encoded = false;
    }

    void WorldLightingPass::notifySubmitted(const FenceCompletionValue value) noexcept {
        if (not m_resources or not m_resources->m_armed) {
            return;
        }

        auto &attempt = m_resources->m_status[m_resources->m_attempt_slot];
        if (not attempt.m_pending or attempt.m_submission != kFenceValueNone) {
            return;
        }

        attempt.m_submission = value;
        attempt.m_world_generation = m_resources->m_world.m_world_generation;
        attempt.m_accepted_frame = ++m_resources->m_accepted_attempts;
        m_resources->m_accepted_active_elapsed = attempt.m_active_elapsed;
        m_resources->m_armed = false;
    }

    void WorldLightingPass::notifyReadSubmitted(const FenceCompletionValue value) noexcept {
        if (not m_resources) {
            return;
        }

        m_resources->m_inspection.m_last_use = std::max(m_resources->m_inspection.m_last_use, value);

        // Only the attempt this C-stage overlay actually decoded is extended.
        // Extending every earlier pending attempt would chain their reader
        // fences to the newest submission and exhaust the status ring.
        auto &attempt = m_resources->m_status[m_resources->m_attempt_slot];
        if (attempt.m_pending and attempt.m_submission != kFenceValueNone and attempt.m_submission <= value) {
            attempt.m_read_submission = std::max(attempt.m_read_submission, value);
        }
    }

    void WorldLightingPass::discardUnsubmitted() noexcept {
        if (not m_resources) {
            return;
        }

        m_resources->m_armed = false;
        for (auto &attempt: m_resources->m_status) {
            // Only definitely-unsubmitted attempts are released; an ambiguous
            // submission stays quarantined with its slot and resources.
            if (attempt.m_pending and attempt.m_submission == kFenceValueNone) {
                clearAttempt_(attempt);
            }
        }
    }

    std::error_code WorldLightingPass::inspectAttempt_(ProjectionStatusSlot &slot) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            slot.m_copy_encoded ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, slot.m_readback_mapped != nullptr
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::state_not_recoverable));

        std::array<NumericStatusRecord, kNumericStatusRecords> records{};
        std::memcpy(records.data(), slot.m_readback_mapped, sizeof(records));

        std::error_code first_err{};
        for (u32 record = 0u; record < records.size(); ++record) {
            const NumericStatusRecord &status = records[record];
            const bool invalid = status.m_invalid_index != kNoNumericIndex;
            const bool range = status.m_range_index != kNoNumericIndex;
            if (not invalid and not range) {
                continue;
            }

            const u32 index = invalid ? status.m_invalid_index : status.m_range_index;
            const std::error_code reported = invalid ? make_error_code(std::errc::state_not_recoverable)
                                                     : make_error_code(std::errc::result_out_of_range);
            PPR_LOG(
                WorldLightingPass, error, "world-lighting numeric status failure",
                {{"category", invalid ? "invalid_state" : "range_error"}, {"stage", kNumericStageNames[record]},
                    {"index", index}, {"accepted_frame", slot.m_accepted_frame},
                    {"model_attempt", m_resources->m_attempt_sequence}, {"world_generation", slot.m_world_generation}});
            if (not first_err) {
                first_err = reported;
            }
        }

        clearAttempt_(slot);
        return first_err;
    }

    std::error_code WorldLightingPass::collectCompleted(const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        std::error_code first_err{};
        for (auto &attempt: m_resources->m_status) {
            if (not attempt.m_pending or attempt.m_submission == kFenceValueNone) {
                continue;
            }

            if (not slotFenceComplete(completed, attempt.m_submission)) {
                continue;
            }

            if (attempt.m_read_submission != kFenceValueNone and
                not slotFenceComplete(completed, attempt.m_read_submission)) {
                continue;
            }

            PPR_RETAIN_ERROR_ON_FAIL(WorldLightingPass, first_err, inspectAttempt_(attempt));
        }

        std::ignore = m_inspection_releases.reclaim(completed);
        return first_err;
    }

    // ------------------------------------------------------------------
    // binding
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::bind(rhi::ShaderCursor &cursor) const {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources and m_resources->m_resolved and m_resources->m_fine_ch
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::not_connected));

        const auto &resources = *m_resources;
        const auto &attempt = resources.m_status[resources.m_attempt_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, resources.m_bound_source_materials and resources.m_bound_thermal_lut
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, cursor["g_resolved_cells"].setBinding(
                                   rhi::Binding(resources.m_resolved, projectionRange(resources.m_resolved))));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            cursor["g_fine_ch"].setBinding(rhi::Binding(resources.m_fine_ch, projectionRange(resources.m_fine_ch))));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            cursor["g_source_materials"].setBinding(
                rhi::Binding(resources.m_bound_source_materials, projectionRange(resources.m_bound_source_materials))));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            cursor["g_thermal_lut"].setBinding(
                rhi::Binding(resources.m_bound_thermal_lut, projectionRange(resources.m_bound_thermal_lut))));

        // The numeric-status buffer is a writer-side resource: only the
        // projection/dispatch entry points declare it. Material and inspection
        // consumers include the same lighting records but must not receive an
        // unbound field, so the binding follows the program's actual layout.
        const rhi::ShaderCursor status_cursor = cursor["g_numeric_status"];
        if (status_cursor.isValid()) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass,
                status_cursor.setBinding(rhi::Binding(attempt.m_status, projectionRange(attempt.m_status))));
        }
        return {};
    }

    std::error_code WorldLightingPass::bindFineCH(rhi::ShaderCursor &cursor) const {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            currentFineCHAvailable() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        const auto &resources = *m_resources;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            cursor["g_fine_ch"].setBinding(rhi::Binding(resources.m_fine_ch, projectionRange(resources.m_fine_ch))));
        return {};
    }

    // ------------------------------------------------------------------
    // retained inspection overlay
    // ------------------------------------------------------------------

    bool WorldLightingPass::inspectionAvailable() const noexcept {
        if (not m_resources or
            not m_resources->m_resolved or
            not m_resources->m_fine_ch or
            not m_resources->m_source or
            not m_resources->m_cascades or
            not m_resources->m_inspection.m_program) {
            return false;
        }

        const auto &attempt = m_resources->m_status[m_resources->m_attempt_slot];
        return attempt.m_pending and
               attempt.m_submission != kFenceValueNone and
               attempt.m_jump and
               attempt.m_inputs.m_visible_materials;
    }

    std::error_code WorldLightingPass::ensureInspectionPipeline_(
        const DrawContext &context, const RenderPipelineSignature &signature, const bool encode_srgb) {
        auto &inspection = m_resources->m_inspection;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass,
            inspection.m_program ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        if (inspection.m_key and static_cast<const RenderPipelineSignature &>(*inspection.m_key) != signature) {
            if (inspection.m_pipeline) {
                m_inspection_releases.defer(
                    inspection.m_last_use, [held = std::move(inspection.m_pipeline)]() mutable { held.setNull(); });
            }
            inspection.m_key.reset();
        }

        if (inspection.m_pipeline) {
            return {};
        }

        rhi::ColorTargetDesc target{};
        target.format = signature.colorFormats().front();
        target.enableBlend = false;
        rhi::RenderPipelineDesc description{};
        description.program = inspection.m_program.get();
        description.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        description.targets = &target;
        description.targetCount = 1u;
        description.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
        description.depthStencil.format = rhi::Format::Undefined;
        description.depthStencil.depthTestEnable = false;
        description.depthStencil.depthWriteEnable = false;
        description.rasterizer.cullMode = rhi::CullMode::None;
        description.label = encode_srgb ? "world lighting inspect (shader sRGB)" : "world lighting inspect";
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, context.m_device.createRenderPipeline(description, inspection.m_pipeline.writeRef()));
        inspection.m_key.emplace(signature);
        return {};
    }

    std::error_code WorldLightingPass::encodeInspection_(
        const DrawContext &context, const CameraSnapshot &camera, const LightingInspectionSettings &settings,
        const bool encode_srgb, const float cells_per_metre) {
        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];

        InspectionStageFrame values{};
        values.m_inverse_view_projection = camera.m_invert_view_projection;
        const float width = static_cast<float>(context.m_target_extent.x);
        const float height = static_cast<float>(context.m_target_extent.y);
        values.m_viewport = float4{width, height, 1.0f / width, 1.0f / height};
        values.m_diagnostic = static_cast<u32>(settings.m_diagnostic);
        values.m_cascade_level = settings.m_cascade;
        values.m_encode_srgb = encode_srgb ? 1u : 0u;
        values.m_cells_per_metre = cells_per_metre;

        auto &root = *resources.m_inspection_roots[resources.m_attempt_slot];
        rhi::ShaderCursor cursor{&root};

        const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, cursor[name].setBinding(rhi::Binding(buffer, projectionRange(buffer))));
            return {};
        };

        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, cursor["g_inspect"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, constants.setData(&values, sizeof(values)));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_resolved_cells", resources.m_resolved));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_visuals", attempt.m_inputs.m_visible_materials));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_fine_ch", resources.m_fine_ch));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_cell_source", resources.m_source));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_cascades", resources.m_cascades));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, cursor["g_jump"].setBinding(rhi::Binding(attempt.m_jump)));

        context.m_pass.pushDebugGroup("WorldLighting.Inspect", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };
        context.m_pass.setRenderState({
            .viewports = {context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {context.m_scissor},
            .scissorRectCount = 1u,
            .vertexBuffers = {},
            .indexBuffer = {},
        });
        context.m_pass.bindPipeline(resources.m_inspection.m_pipeline, &root);
        context.m_pass.draw({.vertexCount = 3u});
        return {};
    }

    std::error_code WorldLightingPass::renderInspection(
        const DrawContext &context, const CameraSnapshot &camera, const LightingInspectionSettings &settings,
        const float cells_per_metre) {
        const hal::ProfileScope scope{"App.Pass.WorldLighting.Inspect"};
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, validateLightingInspectionSettings(settings));
        // Every non-screen-addressed view converts unprojected metres to native
        // cells with this factor. A non-finite or non-positive value would index
        // a degenerate lattice, so it fails closed at the boundary.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, std::isfinite(cells_per_metre) and cells_per_metre > 0.0f
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::invalid_argument));
        if (settings.m_diagnostic == ELightingDiagnostic::off) {
            return {};
        }

        if (not m_ready) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, m_shaders.isValid() and not m_root.empty()
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_connected));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, initialize(*m_shaders, m_root));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources and
                                       m_resources->m_resolved and
                                       m_resources->m_fine_ch and
                                       m_resources->m_source and
                                       m_resources->m_cascades and
                                       m_resources->m_inspection.m_program
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::not_connected));

        // The overlay decodes the attempt this frame actually submitted. A
        // never-submitted or already-collected attempt has no matching world
        // state to stamp, so the caller's view is not fabricated.
        const auto &attempt = m_resources->m_status[m_resources->m_attempt_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, attempt.m_pending and
                                       attempt.m_submission != kFenceValueNone and
                                       attempt.m_jump and
                                       attempt.m_inputs.m_visible_materials
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::not_connected));

        const RenderPipelineSignature &signature = context.m_render_pipeline_key;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, signature.colorFormats().size() == 1u and
                                       not signature.m_depth_stencil_format.has_value() and
                                       signature.m_sample_count == SampleCount::x1 and
                                       context.m_target_extent.x > 0 and
                                       context.m_target_extent.y > 0
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::invalid_argument));

        // A hardware sRGB attachment encodes once; an SDR UNorm attachment asks
        // the overlay shader for the single encoding. Float targets stay as the
        // inspection curve produced them.
        const rhi::Format format = signature.colorFormats().front();
        const bool encode_srgb = format == rhi::Format::RGBA8Unorm or format == rhi::Format::BGRA8Unorm;
        const bool srgb_target = format == rhi::Format::RGBA8UnormSrgb or format == rhi::Format::BGRA8UnormSrgb;
        const bool float_target = format == rhi::Format::RGBA16Float or format == rhi::Format::RGBA32Float;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, encode_srgb or srgb_target or float_target
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::not_supported));

        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, ensureInspectionPipeline_(context, signature, encode_srgb));
        return encodeInspection_(context, camera, settings, encode_srgb, cells_per_metre);
    }

    // ------------------------------------------------------------------
    // tests-only observations
    // ------------------------------------------------------------------

    rhi::IBuffer *WorldLightingPass::resolvedCellsForTest() const noexcept {
        return m_resources ? m_resources->m_resolved.get() : nullptr;
    }

    rhi::IBuffer *WorldLightingPass::accelerationVisibilityForTest() const noexcept {
        if (not m_resources) {
            return nullptr;
        }

        return m_resources->m_status[m_resources->m_attempt_slot].m_visibility.get();
    }

    rhi::ITexture *WorldLightingPass::accelerationJumpForTest() const noexcept {
        if (not m_resources) {
            return nullptr;
        }

        return m_resources->m_status[m_resources->m_attempt_slot].m_jump.get();
    }

    std::span<u32> WorldLightingPass::pendingStatusWordsForTest() noexcept {
        if (not m_resources) {
            return {};
        }

        auto &attempt = m_resources->m_status[m_resources->m_attempt_slot];
        if (attempt.m_readback_mapped == nullptr) {
            return {};
        }

        return {static_cast<u32 *>(attempt.m_readback_mapped), kNumericStatusRecords * kNumericStatusWords};
    }

    bool WorldLightingPass::statusSlotPendingForTest(const u32 slot) const noexcept {
        return m_resources and slot < kStatusRingSize and m_resources->m_status[slot].m_pending;
    }

    bool WorldLightingPass::statusSlotSubmittedForTest(const u32 slot) const noexcept {
        return m_resources and slot < kStatusRingSize and m_resources->m_status[slot].m_submission != kFenceValueNone;
    }

    rhi::IBuffer *WorldLightingPass::cascadeIntervalsForTest() const noexcept {
        return m_resources ? m_resources->m_cascades.get() : nullptr;
    }

    rhi::IBuffer *WorldLightingPass::sourceCellsForTest() const noexcept {
        return m_resources ? m_resources->m_source.get() : nullptr;
    }

    rhi::IBuffer *WorldLightingPass::fineCHForTest() const noexcept {
        return m_resources ? m_resources->m_fine_ch.get() : nullptr;
    }

    u64 WorldLightingPass::acceptedFrameForTest() const noexcept {
        return m_resources ? m_resources->m_accepted_attempts : 0u;
    }

    void WorldLightingPass::setAcceptedFrameForTest(const u64 frame) noexcept {
        if (m_resources) {
            m_resources->m_accepted_attempts = frame;
        }
    }

    i32 WorldLightingPass::lastPreparedSkyHourForTest() const noexcept {
        return m_resources ? m_resources->m_last_sky_hour : -1;
    }
}
