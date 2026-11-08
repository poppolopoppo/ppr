module;

#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>

module game.colony.smoke;

import engine.app;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.sim;
import game.colony.content;
import game.colony.worldgen;
import game.colony.sim_packet;
import std;

namespace pP::colony {
    // TEMPORARY CPU-phase localization; the existing consumer flushes each marker.
    PPR_DEFINE_LOG_CATEGORY(LightingScene, info, immediate)

    namespace {
        // ------------------------------------------------------------------
        // bounded authored reference scene
        // ------------------------------------------------------------------

        constexpr u32 kSceneWidth = 64u, kSceneHeight = 36u;
        constexpr u32 kRasterWidth = 1920u, kRasterHeight = 1080u;
        constexpr u32 kOutputWidth = 3840u, kOutputHeight = 2160u;
        constexpr float kExposureEv = 3.0f;

        struct SceneContent {
            GridStagedFrame m_stage{};
            std::shared_ptr<const lighting::TerrainReceiverSet> m_receivers{};
            u32 m_lamp_id{};
        };

        [[nodiscard]] Expected<SceneContent> makeScene(
            const ContentPresentation &presentation, const std::string_view profile, const bool door_open,
            const u64 revision, const bool environment = false, const bool backwall_albedo = false) {
            const auto &catalog = *presentation.catalog();
            auto captured = captureCatalogPresentation(presentation, profile, 0u, 1u, 1.0);
            PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, captured);

            // Match the existing Grid shader's piecewise transfer. The math
            // module's Mango conversions approximate this transfer instead.
            const auto decode_srgb = [](const double encoded) {
                return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
            };

            const auto encode_srgb = [](const double linear) {
                return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
            };

            constexpr std::string_view keys[]{
                "sandstone", "sand", "regolith", "iron_ore", "copper_ore", "magma", "crystal", "water"};
            const u32 palette_count = environment ? 8u : 6u;
            std::vector<GridTerrainVisual> palette(palette_count);
            std::vector<u16> ids(palette_count);
            u32 largest_id{};
            for (const auto &material: catalog.materials()) {
                largest_id = std::max(largest_id, u32{material.m_numeric_id});
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                LightingScene, largest_id < 65535u - (palette_count - 5u)
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::value_too_large));

            for (u32 index = 0u; index < palette_count; ++index) {
                const auto *const material = catalog.findMaterial(keys[index]);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene,
                    material != nullptr ? std::error_code{} : std::make_error_code(std::errc::no_message));

                const auto visual = std::ranges::find(
                    captured->m_terrain_visuals, material->m_numeric_id, &GridTerrainVisual::m_material_id);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene, visual != captured->m_terrain_visuals.end()
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::no_message));

                palette[index] = *visual;
                ids[index] = material->m_numeric_id;
            }

            // This distinct scene-only lamp is NOT the shipped magma material.
            // Keep its captured texture; author an industrial amber source from
            // sRGB#FFCA75, with linear red S=2.5 under this opaque scene policy.
            auto &lamp = palette[5u];
            lamp.m_material_id = largest_id + 1u;
            lamp.m_color_rgba = 0xffca75ffu;
            lamp.m_emissive_strength = 2.5f;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const u32 shift = 24u - channel * 8u;
                const double encoded = static_cast<double>((lamp.m_color_rgba >> shift) & 255u) / 255.0;
                lamp.m_emissive_rgb[channel] = static_cast<float>(decode_srgb(encoded));
            }

            lamp.m_opacity = 1.0f;
            lamp.m_surface_flags &= ~(
                kGridSurfaceOpticalBlend | kGridSurfaceParticleSolid | kGridSurfaceParticleLiquid);
            lamp.m_transport_model = kGridTransportOpaqueV1;
            std::ranges::fill(lamp.m_sigma_per_m, 0.0f);
            ids[5u] = safe_narrowing(lamp.m_material_id);

            // These explicit scene-only opaque source sculptures retain the
            // captured texture, normal and authored linear emission product.
            // They are not the catalog's absorbing crystal or particle water.
            if (environment) {
                for (u32 index = 6u; index < palette_count; ++index) {
                    auto &source = palette[index];
                    source.m_material_id = largest_id + index - 4u;
                    source.m_color_rgba = (source.m_color_rgba & 0xffffff00u) | 255u;
                    source.m_opacity = 1.0f;
                    source.m_surface_flags &= ~(
                        kGridSurfaceOpticalBlend | kGridSurfaceParticleSolid | kGridSurfaceParticleLiquid);
                    source.m_transport_model = kGridTransportOpaqueV1;
                    source.m_refraction_depth_m = 0.0f;
                    std::ranges::fill(source.m_sigma_per_m, 0.0f);
                    ids[index] = safe_narrowing(source.m_material_id);
                }
            }

            SceneContent result{};
            result.m_lamp_id = lamp.m_material_id;
            auto &stage = result.m_stage;
            stage.m_has_catalog = true;
            stage.m_cell_size_m = 1.0f;
            stage.m_boundary_material_id = 0u;
            stage.m_reference_thickness_m = 1.0f;
            stage.m_has_background = captured->m_has_background;
            stage.m_background_layers = captured->m_background_layers;
            stage.m_terrain_images = std::move(captured->m_terrain_images);
            stage.m_terrain_visuals.assign(palette.begin(), palette.end());

            // Scene-owned row tints darken the same layered textures before EV3.
            // Decode/multiply/re-encode RGB; alpha and parallax remain captured.
            const std::array<double, 3u> background_linear_multiplier = backwall_albedo
                                                                            ? std::array<double, 3u>{0.30, 0.36, 0.45}
                                                                            : std::array<double, 3u>{0.05, 0.11, 0.25};
            for (auto &layer: stage.m_background_layers) {
                u32 tint = layer.m_color_rgba & 255u;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const u32 shift = 24u - channel * 8u;
                    const double encoded = static_cast<double>((layer.m_color_rgba >> shift) & 255u) / 255.0;
                    const double linear = decode_srgb(encoded) * background_linear_multiplier[channel];
                    const auto byte = static_cast<u32>(std::lround(encode_srgb(linear) * 255.0));
                    tint |= byte << shift;
                }

                layer.m_color_rgba = tint;
            }

            std::vector<lighting::OpticalTerrainMaterial> optics{{lighting::OpticalCell{}, 0u, 0.0f}};
            optics.front().m_optics.m_kind = lighting::OpticalKind::vacuum;
            for (const auto &visual: palette) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene, (visual.m_surface_flags & kGridSurfaceOpticalBlend) == 0u
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_supported));

                const auto cell = gridMaterialOpticalCell(visual);
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, cell);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene, cell->m_kind == lighting::OpticalKind::opaque
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_supported));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene,
                    visual.m_material_id == lamp.m_material_id
                        ? (cell->m_surface_emission[0u] > 0.0 ? std::error_code{}
                                                              : std::make_error_code(std::errc::no_message))
                    : visual.m_material_id > lamp.m_material_id
                        ? (std::ranges::any_of(cell->m_surface_emission, [](const double value) { return value > 0.0; })
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::no_message))
                        : (cell->m_surface_emission == lighting::OpticalRgb{}
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::not_supported)));

                optics.push_back({*cell, visual.m_material_id, visual.m_roundness});
            }

            const auto authored_labels = lightingSceneLabels(ids, door_open, environment);
            PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, authored_labels);
            const auto &labels = *authored_labels;

            // Shared source discontinuities are excluded by TerrainReceiverSet:
            // each emitter's adjacent sample neighbourhood is vacuum or same-S.
            const lighting::OpticalTerrainView view{
                labels, optics, {0.0, 0.0}, 1.0, kSceneWidth, kSceneHeight, 0u, lighting::OpticalBoundary::vacuum};
            auto geometry = lighting::OpticalTerrainSnapshot::capture(view);
            PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, geometry);

            auto owner = std::make_shared<const lighting::OpticalTerrainSnapshot>(std::move(*geometry));
            auto receivers = lighting::TerrainReceiverSet::capture(owner);
            PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, receivers);
            result.m_receivers = std::make_shared<const lighting::TerrainReceiverSet>(std::move(*receivers));
            stage.m_optical_geometry = std::move(owner);
            stage.m_optical_revision = revision;

            sim::definition::SemanticFingerprint fingerprint{catalog.definition()};
            fingerprint.string(
                environment ? "ppr.handcrafted.lighting-environment.v1"
                            : "ppr.handcrafted.lighting-scene.amber-navy.v2");
            if (backwall_albedo)
                fingerprint.string("physical-backwall-albedo-v1");
            fingerprint.integer(presentation.fingerprint());
            fingerprint.integer(kSceneWidth);
            fingerprint.integer(kSceneHeight);
            fingerprint.integer(lamp.m_material_id);
            fingerprint.integer(std::bit_cast<u32>(kExposureEv));
            for (const auto label: labels) {
                fingerprint.integer(label);
            }

            stage.m_content_fingerprint = fingerprint.hash();
            // Complete optical geometry replaces every prior resident sample.
            stage.m_reset_caches = true;
            stage.m_full_refresh = true;
            stage.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_chunk_id = 0u;
            upload.m_payload.m_rect[2u] = upload.m_payload.m_rect[3u] = 128.0f;
            upload.m_cell_materials.resize(GridPass::kCellsPerChunk, 0u);
            for (u32 y = 0u; y < kSceneHeight; ++y) {
                std::copy_n(labels.begin() + y * kSceneWidth, kSceneWidth, upload.m_cell_materials.begin() + y * 128u);
            }

            stage.m_uploads.push_back(std::move(upload));
            return result;
        }

        // ------------------------------------------------------------------
        // completed receiver observation
        // ------------------------------------------------------------------

        [[nodiscard]] Expected<double> observedDoorRadiance(
            const lighting::TerrainReceiverSet &receivers,
            const std::span<const lighting::TerrainReceiverResult> output, const bool environment = false) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                LightingScene, output.size() == receivers.features().size()
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::protocol_error));

            double total{};
            u32 count{};
            for (u32 index = 0u; index < output.size(); ++index) {
                const auto &row = output[index];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene,
                    row.m_status == 0u and
                            std::ranges::all_of(
                                row.m_mean, [](const float value) { return std::isfinite(value) and value >= 0.0f; })
                        ? std::error_code{}
                        : std::make_error_code(std::errc::argument_out_of_domain));

                const auto &feature = receivers.features()[index];
                if (environment or (feature.m_visual_x >= 38.0f and
                                       feature.m_visual_x <= 45.0f and
                                       feature.m_visual_y >= 6.0f and
                                       feature.m_visual_y <= 10.0f)) {
                    total += static_cast<double>(row.m_mean[0u]);
                    ++count;
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                LightingScene, count > 0u ? std::error_code{} : std::make_error_code(std::errc::no_message));
            return total / count;
        }

        // ------------------------------------------------------------------
        // frozen game capture owner
        // ------------------------------------------------------------------

        class LightingSceneApp final : public Application {
            // SIMD-containing passes precede pointer/string policy state.
            // GPU resources are released explicitly after a teardown proof.
            GridPass m_grid{};
            RetroPass m_picture{};
            lighting::RadianceCascadePass m_radiance{};
            std::shared_ptr<const ContentPresentation> m_presentation;
            std::filesystem::path m_directory;
            std::string m_profile;
            rhi::ComPtr<rhi::ITexture> m_target{};
            rhi::ComPtr<rhi::IQueryPool> m_grid_queries{};
            u64 m_grid_timestamp_frequency{};
            u32 m_timestamp_cursor{};
            std::error_code m_shutdown_error{};
            bool m_qualified_fp64{}, m_base_started{}, m_grid_started{}, m_picture_started{}, m_radiance_started{},
                m_complete{};

        public:
            LightingSceneApp(
                std::shared_ptr<const ContentPresentation> presentation, std::string profile,
                std::filesystem::path directory, const bool qualified_fp64)
                : Application(
                      ApplicationDomain{
                          .m_is_headless = true,
                          .m_is_interactive = false,
                          .m_needs_presence = false,
                          .m_needs_rendering = true,
                          .m_needs_user_interface = false},
                      "lighting-scene", std::span<const char *const>{}),
                  m_presentation(std::move(presentation)), m_directory(std::move(directory)),
                  m_profile(std::move(profile)), m_qualified_fp64(qualified_fp64) {
            }

            [[nodiscard]] bool complete() const noexcept {
                return m_complete;
            }

            [[nodiscard]] std::error_code shutdownError() const noexcept {
                return m_shutdown_error;
            }

        protected:
            // ------------------------------------------------------------------
            // startup and scene publication
            // ------------------------------------------------------------------

            std::error_code initialize() override {
                m_base_started = true;
                PPR_LOG(LightingScene, info, "SCENE-PHASE before Application initialize");
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, Application::initialize());
                PPR_LOG(LightingScene, info, "SCENE-PHASE after Application initialize");

                const safe_ptr<IRhiService> rhi{getServices().inject()};
                const safe_ptr<IShaderService> shaders{getServices().inject()};
                auto &device = rhi->getDevice();
                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, device.getDeviceType() == rhi::DeviceType::Vulkan and
                                           device.hasFeature("standard-sample-locations-4") and
                                           device.hasFeature("sample-rate-shading")
                                       ? std::error_code{}
                                       : std::make_error_code(std::errc::not_supported));

                std::error_code error;
                std::filesystem::create_directories(m_directory, error);
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, error);

                m_grid_started = true;
                PPR_LOG(LightingScene, info, "SCENE-PHASE before GridPass initialize");
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_grid.initialize(*rhi, *shaders, getContentDir().path()));
                PPR_LOG(LightingScene, info, "SCENE-PHASE after GridPass initialize");

                m_picture_started = true;
                PPR_LOG(LightingScene, info, "SCENE-PHASE before RetroPass initialize");
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_picture.initialize(*rhi, *shaders, getContentDir().path()));
                PPR_LOG(LightingScene, info, "SCENE-PHASE after RetroPass initialize");

                lighting::TerrainPrecisionSelection precision{};
                if (m_qualified_fp64) {
                    precision.m_precision = lighting::TerrainPrecision::qualified_fp64_refinement_experimental;
                    precision.m_rte64_qualified_for_device_session = true;
                }

                m_radiance_started = true;
                PPR_LOG(LightingScene, info, "SCENE-PHASE before RadianceCascadePass initialize");
                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, m_radiance.initialize(device, *shaders, getContentDir().path(), precision));
                PPR_LOG(LightingScene, info, "SCENE-PHASE after RadianceCascadePass initialize");

                rhi::TextureDesc target{};
                target.size = {kOutputWidth, kOutputHeight, 1u};
                target.format = rhi::Format::RGBA8Unorm;
                target.usage = static_cast<rhi::TextureUsage>(
                    static_cast<u32>(rhi::TextureUsage::RenderTarget) |
                    static_cast<u32>(rhi::TextureUsage::CopySource));
                target.defaultState = rhi::ResourceState::RenderTarget;
                target.label = "bounded RC lighting scene SDR capture";
                PPR_LOG(LightingScene, info, "SCENE-PHASE before output createTexture");
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, device.createTexture(target, nullptr, m_target.writeRef()));

                if (device.hasFeature(rhi::Feature::TimestampQuery) and device.getInfo().timestampFrequency != 0u) {
                    rhi::QueryPoolDesc queries{};
                    queries.type = rhi::QueryType::Timestamp;
                    queries.count =
                        128u; // Fields, exact-view reconstruction and matched pictures; no query-slot reuse.
                    queries.label = "lighting scene Grid terrain comparison";
                    PPR_RETURN_ERROR_ON_FAIL(LightingScene, device.createQueryPool(queries, m_grid_queries.writeRef()));
                    m_grid_timestamp_frequency = device.getInfo().timestampFrequency;
                }

                PPR_LOG(LightingScene, info, "SCENE-PHASE initialize return success");
                return {};
            }

            std::error_code update(const TimeSpan dt) override {
                return Application::update(dt);
            }

            std::error_code publishScene_(const SceneContent &scene, const u64 revision, rhi::IDevice &device) {
                auto &renderer = getRenderer();
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, renderer.waitOnHost());
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, renderer.retireCompletedFrames());

                // Retirement is incremental even after idle. Drain its existing
                // bounded work rather than bypassing world-bundle ownership.
                const u32 retirement_bound = 4u * sim::kChunkCount +
                                             static_cast<u32>(m_presentation->textures().size()) +
                                             8u;
                u32 retirement_step{};
                while (not m_grid.worldCandidateAdmissionReady(renderer.completedFenceValue())) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        LightingScene, ++retirement_step <= retirement_bound
                                           ? std::error_code{}
                                           : std::make_error_code(std::errc::state_not_recoverable));
                }

                PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_grid.stageWorldCandidate(revision, revision, scene.m_stage));
                for (;;) {
                    const auto prepared = m_grid.advanceWorldCandidate(device, renderer.completedFenceValue());
                    PPR_RETURN_ERROR_ON_FAIL(LightingScene, prepared);
                    if (*prepared) {
                        break;
                    }

                    PPR_RETURN_ERROR_ON_FAIL(LightingScene, renderer.waitOnHost());
                    PPR_RETURN_ERROR_ON_FAIL(LightingScene, renderer.retireCompletedFrames());
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene,
                    m_grid.activateWorldCandidate(revision, revision, scene.m_stage.m_content_fingerprint));
                return {};
            }

            // ------------------------------------------------------------------
            // actual RC producer and reconstructed game picture
            // ------------------------------------------------------------------

            [[nodiscard]] Expected<double> captureState_(
                const bool open, const u64 revision, const std::string_view name, std::ostream &manifest,
                const bool environment = false, const bool backwall = false) {
                PPR_LOG(LightingScene, info, "SCENE-PHASE before makeScene", {{"state", name}});
                const auto scene = makeScene(*m_presentation, m_profile, open, revision, environment, backwall);
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, scene);
                PPR_LOG(LightingScene, info, "SCENE-PHASE after makeScene", {{"state", name}});

                const safe_ptr<IRhiService> rhi{getServices().inject()};
                auto &device = rhi->getDevice();
                auto &renderer = getRenderer();
                PPR_LOG(LightingScene, info, "SCENE-PHASE before publishScene", {{"state", name}});
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, publishScene_(*scene, revision, device));
                PPR_LOG(LightingScene, info, "SCENE-PHASE after publishScene", {{"state", name}});

                lighting::RcEstimateRequest request{};
                request.m_interval_bytes = 32u;
                request.m_optical_cell_bytes = 40u;
                request.m_width = request.m_c0_tile_width = kSceneWidth;
                request.m_height = request.m_c0_tile_height = kSceneHeight;
                request.m_live_generations = 1u;
                request.m_cascade_count = 1u;
                u64 outer = 1u;
                while (double(outer) < std::hypot(double(kSceneWidth), double(kSceneHeight))) {
                    outer *= 2u;
                    ++request.m_cascade_count;
                }

                // Conservative caller reserve for display/appearance/headroom,
                // not measured VRAM. Product time/age budgets remain unqualified.
                request.m_budget_bytes = 4ull * 1024u * 1024u * 1024u;
                request.m_additional_peak_bytes = 1024ull * 1024u * 1024u;
                request.m_additional_largest_resource_bytes = u64{kOutputWidth} * kOutputHeight * 8u;
                request.m_max_resource_bytes = 512ull * 1024u * 1024u;

                const auto identity = m_grid.activeWorldIdentity();
                const lighting::TerrainReceiverCohort cohort{
                    identity.m_job_id, identity.m_world_generation, identity.m_content_fingerprint, revision};
                PPR_LOG(LightingScene, info, "SCENE-PHASE before prepareTerrainReceivers", {{"state", name}});
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene, m_radiance.prepareTerrainReceivers(
                                       scene->m_receivers, cohort, request, renderer.completedFenceValue(), true,
                                       lighting::TerrainReceiverOutput::mean_only,
                                       backwall ? lighting::TerrainBackgroundOutput::c0_mean
                                                : lighting::TerrainBackgroundOutput::disabled));
                PPR_LOG(LightingScene, info, "SCENE-PHASE after prepareTerrainReceivers", {{"state", name}});

                // The output target supplies the existing Renderer
                // submission boundary; all lighting work is the real RC pass.
                const u32 field_query = m_timestamp_cursor;
                m_timestamp_cursor += 2u;
                const auto encode = [&](const PreRenderContext &context) -> std::error_code {
                    if (m_grid_queries)
                        context.m_encoder.writeTimestamp(m_grid_queries, field_query);
                    PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_radiance.encodeCascades(context));
                    if (m_grid_queries)
                        context.m_encoder.writeTimestamp(m_grid_queries, field_query + 1u);
                    return {};
                };

                const auto before_rc = renderer.lastSubmittedFenceValue();
                PPR_LOG(LightingScene, info, "SCENE-PHASE before RC renderToTexture", {{"state", name}});
                const auto produced = renderer.renderToTexture(*m_target, {}, {}, PreRenderCallback{encode});
                PPR_LOG(LightingScene, info, "SCENE-PHASE after RC renderToTexture", {{"state", name}});
                const auto rc_submission = renderer.lastSubmittedFenceValue();
                if (rc_submission > before_rc) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, m_radiance.notifySubmitted(rc_submission));
                }

                if (produced and rc_submission <= before_rc) {
                    if (renderer.lastSubmitSettlement() == FrameSettlement::kSubmitFailed) {
                        PPR_LOG_WARNING_ON_FAIL(LightingScene, m_radiance.notifySubmitFailed());
                    } else {
                        // renderToTexture abandoned its unsubmitted encoder.
                        PPR_LOG_WARNING_ON_FAIL(LightingScene, m_radiance.discardUnsubmitted());
                    }
                }

                // Accepted submissions stay fenced even if a later operation
                // reported failure; teardown must still prove completion.
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, produced);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene, rc_submission > before_rc ? std::error_code{}
                                                             : std::make_error_code(std::errc::state_not_recoverable));

                PPR_LOG(LightingScene, info, "SCENE-PHASE before RC waitOnHost", {{"state", name}});
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, renderer.waitOnHost());
                PPR_LOG(LightingScene, info, "SCENE-PHASE after RC waitOnHost", {{"state", name}});
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, renderer.retireCompletedFrames());

                const auto log_gpu_time = [&](const u32 query, const std::string_view phase) -> std::error_code {
                    if (not m_grid_queries) {
                        manifest << std::format("state={} phase={} gpu_time=unavailable\n", name, phase);
                        return {};
                    }

                    std::array<u64, 2u> ticks{};
                    PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_grid_queries->getResult(query, 2u, ticks.data()));
                    PPR_RETURN_ERROR_ON_FAIL(
                        LightingScene,
                        ticks[1] >= ticks[0] ? std::error_code{} : std::make_error_code(std::errc::protocol_error));
                    const double milliseconds = 1000.0 *
                                                static_cast<double>(ticks[1] - ticks[0]) /
                                                static_cast<double>(m_grid_timestamp_frequency);
                    manifest << std::format("state={} phase={} gpu_ms={}\n", name, phase, milliseconds);
                    return {};
                };
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, log_gpu_time(field_query, "world_rc_generation"));

                // TEMPORARY: preserve raw failed dependencies before the normal
                // receiver validator rejects them. No generation is published.
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene,
                    m_radiance.writeTerrainCascadeReadbackForQualification(
                        m_directory / (std::string{name} + "-cascade-diagnostic"), renderer.completedFenceValue()));

                PPR_LOG(LightingScene, info, "SCENE-PHASE before readTerrainReceivers", {{"state", name}});
                const auto results = m_radiance.readTerrainReceivers(renderer.completedFenceValue());
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, results);
                PPR_LOG(LightingScene, info, "SCENE-PHASE after readTerrainReceivers", {{"state", name}});

                const auto observed = observedDoorRadiance(*scene->m_receivers, *results, environment);
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, observed);

                const auto generation = m_radiance.acquireTerrainReceiverView(renderer.completedFenceValue());
                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, generation);

                // Statistics belong to this accepted immutable generation,
                // never an unfinished or subsequently prepared producer state.
                if (const auto *const statistics = (*generation)->precisionStatistics()) {
                    manifest << std::format(
                        "state={} precision_statistics=completed_generation query_upper_bound={} attempted={} qualified={} unresolved={} bound_error={}\n",
                        name, statistics->m_query_upper_bound, statistics->m_attempted, statistics->m_qualified,
                        statistics->m_unresolved, statistics->m_bound_error);
                } else {
                    manifest << std::format(
                        "state={} precision_statistics=absent mode={}\n", name,
                        m_qualified_fp64 ? "qualified_fp64_refinement_experimental" : "strict_fp32_qualification");
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, m_grid.setTerrainReceiverLighting(*generation));

                std::map<u32, u64> candidate_counts{};
                u64 candidate_total{};
                u32 candidate_max{};
                for (const auto &range: scene->m_receivers->components()) {
                    ++candidate_counts[range.m_count];
                    candidate_total += range.m_count;
                    candidate_max = std::max(candidate_max, range.m_count);
                }

                manifest << std::format(
                    "state={} candidate_ranges={} candidate_total={} candidate_max={} candidate_storage_bytes={} preparation_scope=App.Lighting.CaptureTerrainReceivers\n",
                    name, scene->m_receivers->components().size(), candidate_total, candidate_max,
                    scene->m_receivers->candidates().size_bytes());
                for (const auto &[count, ranges]: candidate_counts) {
                    manifest << std::format("candidate_count={} ranges={}\n", count, ranges);
                }

                Camera camera{ECameraProjection::orthographic};
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_origin = float3{0.0f, 0.0f, -0.5f};
                model.m_ortho_scale = float(kSceneWidth) / float(kRasterWidth);
                camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, kRasterWidth, kRasterHeight}});

                std::shared_ptr<const lighting::TerrainBackgroundGpuView> background_view{};
                if (backwall) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        LightingScene,
                        m_radiance.prepareTerrainBackgroundView(
                            *generation, camera.getSnapshot(), int2{int(kRasterWidth), int(kRasterHeight)},
                            request.m_budget_bytes, request.m_additional_peak_bytes, request.m_max_resource_bytes,
                            renderer.completedFenceValue()));
                    const u32 view_query = m_timestamp_cursor;
                    m_timestamp_cursor += 2u;
                    const auto encode_view = [&](const PreRenderContext &context) -> std::error_code {
                        if (m_grid_queries)
                            context.m_encoder.writeTimestamp(m_grid_queries, view_query);
                        PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_radiance.encodeTerrainBackgroundView(context));
                        if (m_grid_queries)
                            context.m_encoder.writeTimestamp(m_grid_queries, view_query + 1u);
                        return {};
                    };

                    const auto before_view = renderer.lastSubmittedFenceValue();
                    const auto encoded_view = renderer.renderToTexture(
                        *m_target, {}, {}, PreRenderCallback{encode_view});
                    const auto view_submission = renderer.lastSubmittedFenceValue();
                    if (view_submission > before_view) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, m_radiance.notifySubmitted(view_submission));
                    } else if (encoded_view) {
                        if (renderer.lastSubmitSettlement() == FrameSettlement::kSubmitFailed) {
                            PPR_LOG_WARNING_ON_FAIL(LightingScene, m_radiance.notifySubmitFailed());
                        } else {
                            PPR_LOG_WARNING_ON_FAIL(LightingScene, m_radiance.discardUnsubmitted());
                        }
                    }
                    PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, encoded_view);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        LightingScene, view_submission > before_view
                                           ? std::error_code{}
                                           : std::make_error_code(std::errc::state_not_recoverable));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, renderer.waitOnHost());
                    PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, renderer.retireCompletedFrames());
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        LightingScene, log_gpu_time(view_query, "exact_view_background_reconstruction"));
                    const auto acquired_view = m_radiance.acquireTerrainBackgroundView(renderer.completedFenceValue());
                    PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, acquired_view);
                    background_view = *acquired_view;
                    PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, m_grid.setTerrainBackgroundLighting(background_view));
                    const auto &stats = background_view->statistics();
                    manifest << std::format(
                        "state={} backwall_plane_cells=[.5,63.5]x[.5,35.5] outside=far_void field_stride=16 view_positions=5 view_bytes={} attempted={} qualified={} unresolved={} bound_error={} outside_plane={} samples={} invalid_values={} albedo_multiplier_linear=(.30,.36,.45) lighting_worldXY=physical_plane parallax=textureUV_only\n",
                        name, background_view->bufferBytes(), stats.m_attempted, stats.m_qualified, stats.m_unresolved,
                        stats.m_bound_error, stats.m_outside_plane, stats.m_samples, stats.m_invalid_values);
                }

                // Diagnostic comparison pictures share this completed immutable
                // field. The companion recipe captures nearest only. No RC rerun.
                for (const auto continuation:
                    {GridPass::ReceiverContinuation::nearest,
                        GridPass::ReceiverContinuation::interpolated_experiment}) {
                    const bool interpolated = continuation == GridPass::ReceiverContinuation::interpolated_experiment;
                    if (environment and interpolated) {
                        continue; // The companion scene retains the existing nearest control.
                    }

                    if (backwall and interpolated)
                        continue;

                    for (const bool lit: {false, true}) {
                        if (not backwall and lit)
                            continue;

                        const std::string picture_name = std::string{name} +
                                                         (interpolated ? "-interpolated" : "") +
                                                         (backwall ? (lit ? "-lit" : "-unlit-control") : "");
                        const u32 query = m_timestamp_cursor;
                        m_timestamp_cursor += 4u;

                        const auto raster = m_picture.beginFrame(
                            RetroSettings{
                                .m_hdr_enabled = true, .m_spatial_enabled = true, .m_exposure_ev = kExposureEv},
                            camera.getSnapshot(), int2{int(kRasterWidth), int(kRasterHeight)},
                            int2{int(kOutputWidth), int(kOutputHeight)}, identity.m_world_generation, identity.m_job_id,
                            identity.m_content_fingerprint);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, raster);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            LightingScene,
                            m_grid.update(
                                TimeSpan{}, *raster, true, continuation, GridPass::ReceiverResponse::flat,
                                lit ? GridPass::BackgroundResponse::lit_backwall
                                    : GridPass::BackgroundResponse::unlit));

                        const auto terrain = [&](const DrawContext &context) -> std::error_code {
                            if (m_grid_queries)
                                context.m_pass.writeTimestamp(m_grid_queries, query + 2u);
                            PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_grid.renderBackground(context));
                            if (m_grid_queries)
                                context.m_pass.writeTimestamp(m_grid_queries, query + 3u);

                            if (m_grid_queries)
                                context.m_pass.writeTimestamp(m_grid_queries, query);

                            PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_grid.renderGpuIndirect(context));

                            if (m_grid_queries)
                                context.m_pass.writeTimestamp(m_grid_queries, query + 1u);
                            return {};
                        };

                        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                            PPR_RETURN_ERROR_ON_FAIL(
                                LightingScene, m_grid.prepareBackground(
                                                   context, *raster,
                                                   rhi::Viewport::fromSize(float(kRasterWidth), float(kRasterHeight)),
                                                   rhi::ScissorRect::fromSize(kRasterWidth, kRasterHeight)));
                            PPR_RETURN_ERROR_ON_FAIL(LightingScene, m_grid.prepareGpuVisibility(context));
                            return m_picture.prepareScene(
                                context, RetroPass::scenePipelineSignature(true),
                                int2{int(kRasterWidth), int(kRasterHeight)}, DrawCallback{terrain});
                        };

                        const auto present = [&](const DrawContext &context) -> std::error_code {
                            return m_picture.render(context);
                        };

                        const auto before_picture = renderer.lastSubmittedFenceValue();
                        PPR_LOG(LightingScene, info, "SCENE-PHASE before picture renderToTexture", {{"state", name}});
                        const auto picture_error = renderer.renderToTexture(
                            *m_target, {DrawSubmission{"RC terrain / spatial / exposure / AgX", DrawCallback{present}}},
                            {}, PreRenderCallback{prepare});
                        PPR_LOG(LightingScene, info, "SCENE-PHASE after picture renderToTexture", {{"state", name}});
                        const auto picture_submission = renderer.lastSubmittedFenceValue();
                        if (picture_submission > before_picture) {
                            m_grid.notifyFrameSubmitted(picture_submission);
                            m_picture.notifyFrameSubmitted(picture_submission);
                        }

                        PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, picture_error);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            LightingScene, picture_submission > before_picture
                                               ? std::error_code{}
                                               : std::make_error_code(std::errc::state_not_recoverable));

                        PPR_LOG(LightingScene, info, "SCENE-PHASE before picture waitOnHost", {{"state", name}});
                        PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, renderer.waitOnHost());
                        PPR_LOG(LightingScene, info, "SCENE-PHASE after picture waitOnHost", {{"state", name}});
                        PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, renderer.retireCompletedFrames());

                        PPR_RETURN_UNEXPECTED_ON_FAIL(LightingScene, log_gpu_time(query + 2u, "background_raster"));

                        if (m_grid_queries) {
                            std::array<u64, 2u> ticks{};
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                LightingScene, m_grid_queries->getResult(query, 2u, ticks.data()));
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                LightingScene, ticks[1] >= ticks[0] ? std::error_code{}
                                                                    : std::make_error_code(std::errc::protocol_error));

                            const double milliseconds = 1000.0 *
                                                        static_cast<double>(ticks[1] - ticks[0]) /
                                                        static_cast<double>(m_grid_timestamp_frequency);
                            manifest << std::format(
                                "picture={} grid_terrain_gpu_ms={} raster=1920x1080 samples=4 excludes=background_and_post\n",
                                picture_name, milliseconds);
                        } else {
                            manifest << "grid_terrain_gpu_timing=unavailable; no performance qualification\n";
                        }

                        PPR_LOG(LightingScene, info, "SCENE-PHASE before writeCaptureReadback", {{"state", name}});
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            LightingScene,
                            writeCaptureReadback(device, *m_target, m_directory / (picture_name + ".ppm")));
                        PPR_LOG(LightingScene, info, "SCENE-PHASE after writeCaptureReadback", {{"state", name}});

                        manifest << std::format(
                            "state={} continuation={} door={} job={} world={} geometry_revision={} presentation={:016x} rc_submit={} picture_submit={} receivers={} receiver_bytes={} {}={:.12g} lamp_id={}\n",
                            name, interpolated ? "interpolated_experiment" : "nearest",
                            environment ? "not_applicable"
                            : open      ? "open"
                                        : "closed",
                            identity.m_job_id, identity.m_world_generation, revision, identity.m_content_fingerprint,
                            rc_submission, picture_submission, results->size(), (*generation)->bufferBytes(),
                            environment ? "all_receivers_mean_red" : "right_floor_mean_red", *observed,
                            scene->m_lamp_id);
                    }
                }

                constexpr std::string_view names[]{
                    "sandstone", "sand", "regolith", "iron_ore", "copper_ore", "scene_amber_lamp",
                    "scene_opaque_violet_sculpture", "scene_opaque_cyan_tank_insert"};
                for (u32 index = 0u; index < scene->m_stage.m_terrain_visuals.size(); ++index) {
                    const auto &visual = scene->m_stage.m_terrain_visuals[index];
                    manifest << std::format(
                        "material={} id={} tint={:08x} roundness={} albedo_index={} normal_index={} S=({},{},{})\n",
                        names[index], visual.m_material_id, visual.m_color_rgba, visual.m_roundness,
                        visual.m_texture_index, visual.m_normal_texture_index,
                        visual.m_emissive_rgb[0u] * visual.m_emissive_strength,
                        visual.m_emissive_rgb[1u] * visual.m_emissive_strength,
                        visual.m_emissive_rgb[2u] * visual.m_emissive_strength);
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    LightingScene, manifest ? std::error_code{} : std::make_error_code(std::errc::io_error));
                return *observed;
            }

            std::error_code render() override {
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, Application::render());

                std::ofstream manifest{m_directory / "lighting-scene.txt", std::ios::trunc};
                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, manifest ? std::error_code{} : std::make_error_code(std::errc::io_error));
                manifest << std::format(
                    "handcrafted_lighting_scene_v2 profile={} catalog_presentation={:016x}\nscene=64x36 cell_metres=1 vacuum_ring=1 wall_x=32 wall_thickness=1 aperture_cells=1x8 door_y=13..20 lamp=scene_amber_lamp\nraster=1920x1080 MSAA=4 output=3840x2160 spatial=current_frame_hable temporal=off EV={} tone=Filament_AgX_Punchy_approximation output_encoding=shader_sRGB_once UI=absent\n",
                    m_profile, m_presentation->fingerprint(), kExposureEv);
                manifest << std::format(
                    "precision_mode={} rte64_qualified_for_device_session_operator_assertion={}\n",
                    m_qualified_fp64 ? "qualified_fp64_refinement_experimental" : "strict_fp32_qualification",
                    m_qualified_fp64);
                manifest
                    << "Scene-only opaque lamp retains captured magma texture, with authored sRGB#FFCA75 tint/hue and linear red S=2.5; shipped magma is unchanged. Scene background row RGB is multiplied in linear space by(0.05,0.11,0.25), preserving alpha/textures/parallax. Five sediment/ore rows retain authored tints, textures, normals and roundness. No particle/media, ambient term, radial light list, normal/specular RC, bloom or temporal reconstruction. Nearest/interpolated cut-face comparison shares each completed lighting generation; interpolation is a visibility-unsafe visual experiment until rendered-linear holdouts pass. Baseline art measurement, not final art acceptance. Blocking diagnostic; no frame-budget/full-world acceptance.\n";

                const auto closed = captureState_(false, 1u, "door-closed", manifest);
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, closed);

                const auto open = captureState_(true, 2u, "door-open", manifest);
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, open);

                manifest << std::format("door_response_mean_red_delta={:.12g}\n", *open - *closed);

                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene,
                    *open > *closed ? std::error_code{} : std::make_error_code(std::errc::argument_out_of_domain));

                manifest
                    << "recipe=environment-reference-v1 scene=64x36 cell_metres=1 vacuum_ring=1 continuation=nearest camera_and_EV=diagnostic_control\n"
                    << "composition=pale_sandstone_banks,ochre_sand_shelves,orange_regolith_seams,dark_ore_pockets shaft_x=18..25 platform_x=26..39_y=14 pipe_x=39_y=15..24 tank_bounds=43..52,7..15\n"
                    << "scene_amber_lamp bounds=30..31,21..22 source=authored_amber_S_red_2.5; scene_opaque_violet_sculpture columns=53:23..26,55:23..28,57:23..25 texture_and_S=captured_crystal; scene_opaque_cyan_tank_insert bounds=45..50,9..12 texture_and_S=captured_water\n"
                    << "All three scene sources have model=opaque sigma=(0,0,0) opacity=1 blend=false and a vacuum sample gap. Violet sculpture is not absorbing crystal; cyan tank insert is not water, fluid simulation or media qualification. Shipped material descriptors are unchanged. Industrial silhouettes use captured opaque ore rows and actual RC lighting, without TrianglePass fixed lighting. Authored normal maps remain attached but RC normal relief/specular/bloom remain pending.\n";

                const auto environment = captureState_(false, 3u, "environment-reference", manifest, true);
                PPR_RETURN_ERROR_ON_FAIL(LightingScene, environment);

                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, captureState_(false, 4u, "door-closed-backwall", manifest, false, true));
                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, captureState_(true, 5u, "door-open-backwall", manifest, false, true));
                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, captureState_(false, 6u, "environment-reference-backwall", manifest, true, true));

                manifest.close();
                PPR_RETURN_ERROR_ON_FAIL(
                    LightingScene, manifest ? std::error_code{} : std::make_error_code(std::errc::io_error));

                m_complete = true;
                requestExit();
                return {};
            }

            // ------------------------------------------------------------------
            // inverse acquisition and GPU completion proof
            // ------------------------------------------------------------------

            std::error_code shutdown() override {
                std::error_code first{};
                try {
                    first = prepareGpuTeardown_();
                    if (gpuTeardownDisposition() == GpuQuiescence::kUnknown) {
                        failStopOnUnknownGpuTeardown_(first);
                    }
                } catch (...) {
                    failStopOnUnknownGpuTeardown_(first);
                }

                if (gpuTeardownDisposition() == GpuQuiescence::kRemovalConfirmed) {
                    if (m_picture_started) {
                        PPR_RETAIN_ERROR_ON_FAIL(LightingScene, first, m_picture.notifyDeviceLost());
                    }

                    if (m_grid_started) {
                        PPR_RETAIN_ERROR_ON_FAIL(LightingScene, first, m_grid.notifyDeviceLost());
                    }
                }

                m_target.setNull();
                m_grid_queries.setNull();

                if (m_picture_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(LightingScene, first, m_picture.shutdown());
                    m_picture_started = false;
                }

                if (m_grid_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(LightingScene, first, m_grid.shutdown());
                    m_grid_started = false;
                }

                if (m_radiance_started) {
                    const GpuQuiescenceResult proof{.m_disposition = gpuTeardownDisposition(), .m_error = first};
                    PPR_RETAIN_ERROR_ON_FAIL(LightingScene, first, m_radiance.shutdown(proof));
                    m_radiance_started = false;
                }

                if (m_base_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(LightingScene, first, Application::shutdown());
                    m_base_started = false;
                }

                m_shutdown_error = first;
                return first;
            }
        };
    }

    // ------------------------------------------------------------------
    // explicit game capture entry
    // ------------------------------------------------------------------

    std::error_code captureLightingScene(
        std::shared_ptr<const ContentPresentation> presentation, std::string profile,
        const std::filesystem::path &directory, const bool qualified_fp64) {
        PPR_RETURN_ERROR_ON_FAIL(
            LightingScene, presentation and presentation->catalog() and not directory.empty()
                               ? std::error_code{}
                               : std::make_error_code(std::errc::invalid_argument));

        LightingSceneApp app{std::move(presentation), std::move(profile), directory, qualified_fp64};
        PPR_RETURN_ERROR_ON_FAIL(LightingScene, app.run());
        PPR_RETURN_ERROR_ON_FAIL(LightingScene, app.shutdownError());
        PPR_RETURN_ERROR_ON_FAIL(
            LightingScene, app.complete() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        return {};
    }
}
