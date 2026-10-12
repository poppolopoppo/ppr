module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.grid_pass;
import :renderer.frame_fence;
import :renderer.types;
import :scene.camera;
import std;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(GridPass, debug, none)

    namespace {
        // Never bind a bare rhi::Binding(): the default range does not survive
        // the module boundary and would build an empty SRV view. Every
        // setBinding below goes through makeFullRange.
        [[nodiscard]] rhi::BufferRange makeFullRange(rhi::IBuffer *const buffer) noexcept {
            return rhi::BufferRange{
                .offset = 0u,
                .size = buffer->getDesc().size,
            };
        }

        // Chunk-sized tiles are square in x/y by construction, so the scalar
        // Box size is exact in x/y (mango sizes by the full extent, hence the
        // doubling of the half-extent); the z slab is a conservative half
        // cell so grazing views never drop an in-frustum tile on depth.
        [[nodiscard]] Box tileBox_(const GridTileRange &range, const float cell_size_m) noexcept {
            const float center_x = (static_cast<float>(range.m_min_x) + static_cast<float>(range.m_max_x)) * 0.5f;
            const float center_y = (static_cast<float>(range.m_min_y) + static_cast<float>(range.m_max_y)) * 0.5f;
            const float half_x = (static_cast<float>(range.m_max_x) - static_cast<float>(range.m_min_x)) * 0.5f;
            const float half_y = (static_cast<float>(range.m_max_y) - static_cast<float>(range.m_min_y)) * 0.5f;
            const float half = std::max({half_x, half_y, 0.5f}) + GridPass::kGridCullEpsilon;
            return Box{float3{center_x * cell_size_m, center_y * cell_size_m, 0.0f}, half * 2.0f * cell_size_m};
        }

        void applySubmissionBounds_(GridTilePayload &payload, const GridTileRange &range) noexcept {
            payload.m_rect[0] = static_cast<float>(range.m_min_x);
            payload.m_rect[1] = static_cast<float>(range.m_min_y);
            payload.m_rect[2] = static_cast<float>(range.m_max_x);
            payload.m_rect[3] = static_cast<float>(range.m_max_y);
        }
    }

    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // validation and planning
    // ------------------------------------------------------------------

    std::error_code GridPass::validateSubmission_(const GridTileSubmission &submission) noexcept {
        const GridTileRange &range = submission.m_tile_range;
        if (range.m_max_x <= range.m_min_x or range.m_max_y <= range.m_min_y) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (range.m_min_x < 0 or range.m_min_y < 0 or range.m_max_x > kGridWorldEdge or range.m_max_y > kGridWorldEdge)
            [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        // Chunk-sized tiles use bit 0 only; higher bits name tiles that do
        // not exist, so they fail closed instead of silently uploading nothing.
        if ((submission.m_dirty_mask & ~1u) != 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return default_value_v;
    }

    // Pure, CPU-only, and unit-tested without a device: keeps the submissions
    // whose epsilon-expanded tile boxes intersect the snapshot frustum, in
    // submission order. Quiescent/off-screen tiles never reach the plan.
    Expected<GridTilePlan> GridPass::planTiles(
        const CameraSnapshot &snapshot, const std::span<const GridTileSubmission> submissions,
        const float cell_size_m) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, (cell_size_m == 1.0f or cell_size_m == 1.0f / 3.0f)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        GridTilePlan plan{};
        for (std::size_t index = 0u; index < submissions.size(); ++index) {
            const GridTileSubmission &submission = submissions[index];
            if (const std::error_code err = validateSubmission_(submission)) [[unlikely]] {
                return std::unexpected{err};
            }
            if (not snapshot.m_frustum.isVisible(tileBox_(submission.m_tile_range, cell_size_m))) {
                continue;
            }
            plan.m_tiles.push_back(
                GridPlannedTile{
                    .m_submission_index = static_cast<u32>(index),
                    .m_chunk_id = submission.m_chunk_id,
                    .m_tile_range = submission.m_tile_range,
                    .m_material_id = submission.m_material_id,
                });
        }
        return plan;
    }

    // Pure, CPU-only, and unit-tested without a device: one indirect draw per
    // planned tile in plan order, so the args sequence IS the visible set.
    // Past the tile-cache budget the lane fails closed (no_buffer_space)
    // under the same policy as requestUpload, never a partial args buffer.
    // An empty plan compacts to zero args: zero tiles, zero draws.
    Expected<Array<rhi::IndirectDrawArguments>> GridPass::compactIndirect(const GridTilePlan &plan) {
        if (plan.m_tiles.size() > kGridTileCacheCapacity) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        Array<rhi::IndirectDrawArguments> args{};
        args.reserve(plan.m_tiles.size());
        for (std::size_t index = 0u; index < plan.m_tiles.size(); ++index) {
            args.push_back(
                rhi::IndirectDrawArguments{
                    .vertexCountPerInstance = kGridTileVertexCount,
                    .instanceCount = 1u,
                    .startVertexLocation = 0u,
                    .startInstanceLocation = safe_narrowing(index),
                });
        }
        return args;
    }

    // ------------------------------------------------------------------
    // pass lifecycle and submission APIs
    // ------------------------------------------------------------------

    std::error_code GridPass::initialize(
        IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        bool shader_state_started = false;
        bool initialization_complete = false;
        PPR_DEFER {
            if (not initialization_complete) {
                if (shader_state_started) {
                    m_cached_pipeline.reset();
                    m_render_pipeline_key.reset();
                    m_shader_program.setNull();
                }
                m_ready = false;
            }
        };

        shader_state_started = true;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, createShaderProgram_(shader_service, device, content_dir));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, createVisibilityPrograms_(shader_service, device, content_dir));

        m_ready = true;
        initialization_complete = true;

        PPR_LOG(GridPass, info, "GridPass initialized");
        return default_value_v;
    }

    std::error_code GridPass::update([[maybe_unused]] TimeSpan dt, const CameraSnapshot &camera_view) {
        m_visibility_prepared = false;
        m_camera_view = camera_view;

        return default_value_v;
    }

    std::error_code GridPass::submitTiles(const std::span<const GridTileSubmission> submissions) {
        m_visibility_prepared = false;
        // Pure CPU: staging needs no device, so device-free tests and the
        // colony translator can stage before (or without) initialize; only
        // render touches the GPU.
        for (const GridTileSubmission &submission: submissions) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSubmission_(submission));
        }
        const bool unchanged = std::ranges::equal(
            m_world.m_submitted_tiles, submissions, [](const GridTileSubmission &lhs, const GridTileSubmission &rhs) {
                return lhs.m_chunk_id == rhs.m_chunk_id and
                       lhs.m_material_id == rhs.m_material_id and
                       lhs.m_tile_range.m_min_x == rhs.m_tile_range.m_min_x and
                       lhs.m_tile_range.m_min_y == rhs.m_tile_range.m_min_y and
                       lhs.m_tile_range.m_max_x == rhs.m_tile_range.m_max_x and
                       lhs.m_tile_range.m_max_y == rhs.m_tile_range.m_max_y;
            });
        m_world.m_resident_table_stale = m_world.m_resident_table_stale or not unchanged;
        m_world.m_submitted_tiles.clear();
        m_world.m_submitted_tiles.reserve(submissions.size());
        for (const GridTileSubmission &submission: submissions) {
            m_world.m_submitted_tiles.push_back(submission);
        }
        return default_value_v;
    }

    void GridPass::clearTiles() noexcept {
        m_visibility_prepared = false;
        m_world.m_resident_table_stale = true;
        m_world.m_submitted_tiles.clear();
    }

    void GridPass::clearCache() noexcept {
        m_visibility_prepared = false;
        m_world.m_resident_table_stale = true;
        cancelCellCohort_();
        m_world.m_cell_slots.fill(std::nullopt);
        m_world.m_cell_slots_current.reset();
        m_world.m_cell_publication_deferred = false;
        m_world.m_tile_cache.clear();
        m_world.m_cell_cache.clear();
        // Keep the atlas allocated: its previous contents may still be read
        // by an in-flight draw. A future cell request repopulates its slot
        // only after its last submitted reader fence completes.
    }

    std::error_code GridPass::applyStagedFrame(const GridStagedFrame &stage) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, (stage.m_cell_size_m == 1.0f or stage.m_cell_size_m == 1.0f / 3.0f)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, stage.m_boundary_material_id > 0u and stage.m_boundary_material_id < 0xffffu
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, stage.m_terrain_images.size() <= 256u and stage.m_terrain_visuals.size() <= 65534u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        u64 decoded_bytes = 0u;
        for (const auto &image: stage.m_terrain_images) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, image.m_width > 0u and
                                  image.m_width <= 2048u and
                                  image.m_height > 0u and
                                  image.m_height <= 2048u and
                                  image.m_storage.isOwned() and
                                  image.m_mip_count > 0u and
                                  image.m_mip_count <=
                                      static_cast<u32>(std::bit_width((std::max) (image.m_width, image.m_height))) and
                                  image.m_subresources.size() == image.m_mip_count and
                                  image.m_dimension == image::EImageDimension::image2d and
                                  not image.m_is_block and
                                  image.m_tag == image::EBlockTag::none and
                                  (image.m_format == image::ENativeImageFormat::rgba8_linear or
                                      image.m_format == image::ENativeImageFormat::rgba8_srgb) and
                                  image.m_storage.isMaterialized()
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            const u64 bytes = image.m_storage.getBufferData().size();
            u64 offset = 0u;
            for (u32 level = 0u; level < image.m_mip_count; ++level) {
                const u64 width = image::mipExtentAt(image.m_width, level);
                const u64 height = image::mipExtentAt(image.m_height, level);
                const u64 expected = width * height * 4u;
                const auto &subresource = image.m_subresources[level];
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, offset <= bytes and expected <= bytes - offset
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, subresource.m_view.isMaterialized() and
                                      subresource.m_view.getBufferData().data() ==
                                          image.m_storage.getBufferData().data() + offset and
                                      subresource.m_view.getBufferData().size() == expected and
                                      subresource.m_row_pitch == width * 4u and
                                      subresource.m_slice_pitch == expected
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));
                offset += expected;
            }
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, bytes == offset ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, bytes <= 128u * 1024u * 1024u - decoded_bytes
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            decoded_bytes += bytes;
        }
        std::bitset<65536u> material_ids;
        for (const GridTerrainVisual &visual: stage.m_terrain_visuals) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, visual.m_material_id > 0u and
                                  visual.m_material_id < 0xffffu and
                                  not material_ids.test(visual.m_material_id) and
                                  std::isfinite(visual.m_texture_period_m) and
                                  visual.m_texture_period_m >= 4096.0f / (std::numeric_limits<float>::max)() and
                                  (visual.m_texture_index == 0xffffffffu or
                                      visual.m_texture_index < stage.m_terrain_images.size())
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            material_ids.set(visual.m_material_id);
        }
        if (m_world.m_visuals_set) {
            const bool same = m_world.m_content_fingerprint == stage.m_content_fingerprint and
                              std::ranges::equal(
                                  m_world.m_terrain_images, stage.m_terrain_images,
                                  [](const image::ImageAsset &a, const image::ImageAsset &b) {
                                      return a.m_storage == b.m_storage and
                                             a.m_width == b.m_width and
                                             a.m_height == b.m_height and
                                             a.m_format == b.m_format and
                                             a.m_mip_count == b.m_mip_count;
                                  }) and
                              std::ranges::equal(
                                  m_world.m_terrain_visuals, stage.m_terrain_visuals,
                                  [](const GridTerrainVisual &a, const GridTerrainVisual &b) {
                                      return a.m_material_id == b.m_material_id and
                                             a.m_color_rgba == b.m_color_rgba and
                                             a.m_texture_index == b.m_texture_index and
                                             a.m_texture_period_m == b.m_texture_period_m;
                                  });
            // Catalog changes require an independently prepared world bundle.
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, same ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        } else if (not stage.m_terrain_visuals.empty()) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, m_world.m_terrain_palette == nullptr
                              ? std::error_code{}
                              : std::make_error_code(std::errc::operation_not_permitted));
            m_world.m_terrain_visuals = stage.m_terrain_visuals;
            m_world.m_terrain_images = stage.m_terrain_images;
            m_world.m_content_fingerprint = stage.m_content_fingerprint;
            m_world.m_visuals_set = true;
        }
        if (m_world.m_cell_size_m != stage.m_cell_size_m or
            m_world.m_contours_enabled != stage.m_has_catalog or
            m_world.m_boundary_material_id != stage.m_boundary_material_id) {
            m_world.m_resident_table_stale = true;
        }
        m_world.m_cell_size_m = stage.m_cell_size_m;
        m_world.m_contours_enabled = stage.m_has_catalog;
        m_world.m_boundary_material_id = stage.m_boundary_material_id;
        if (stage.m_reset_caches) {
            clearTiles();
            clearCache();
        }
        PPR_RETURN_ERROR_ON_FAIL(GridPass, stageDiagnostic_(stage));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, submitTiles(std::span<const GridTileSubmission>{stage.m_tiles}));
        for (const GridStagedUpload &upload: stage.m_uploads) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, appendDiagnostic_(upload));
            GridUploadRequest request{};
            request.m_chunk_id = upload.m_chunk_id;
            request.m_tile_data_view = std::span<const GridTilePayload>{&upload.m_payload, 1u};
            request.m_cell_materials = std::span<const u16>{
                upload.m_cell_materials.data(), upload.m_cell_materials.size()};
            const Expected<u32> uploaded = requestUpload(request);
            if (not uploaded) [[unlikely]] {
                clearTiles();
                clearCache();
                return uploaded.error();
            }
            if (*uploaded != 1u) [[unlikely]] {
                clearTiles();
                clearCache();
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }
        return default_value_v;
    }

    std::error_code GridPass::stageWorldCandidate(
        const u64 job_id, const u64 world_generation, const GridStagedFrame &stage, const bool uploads_complete) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, job_id != 0u and world_generation != 0u ? std::error_code{}
                                                              : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, stage.m_diagnostic_revision == 0u or stage.m_diagnostic_revision == job_id
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, not m_candidate_world and not m_retiring_world
                          ? std::error_code{}
                          : std::make_error_code(std::errc::device_or_resource_busy));
        auto candidate = std::make_unique<WorldResources>();
        std::swap(m_world, *candidate);
        bool staged = false;
        PPR_DEFER {
            std::swap(m_world, staged ? *m_candidate_world : *candidate);
            m_resident_table_uploaded.fill(false);
            m_chunk_slot_uploaded.fill(false);
            m_visibility_prepared = false;
        };
        PPR_RETURN_ERROR_ON_FAIL(GridPass, applyStagedFrame(stage));
        m_world.m_job_id = job_id;
        m_world.m_world_generation = world_generation;
        m_world.m_content_fingerprint = stage.m_content_fingerprint;
        m_world.m_candidate_uploads_complete = uploads_complete;
        // The guard restores the active world; retain the candidate owner only
        // after all CPU staging has succeeded.
        m_candidate_world = std::move(candidate);
        staged = true;
        return {};
    }

    std::error_code GridPass::appendWorldCandidate(
        const u64 job_id, const u64 world_generation, const std::span<const GridStagedUpload> uploads,
        const bool final_batch) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_candidate_world and
                              m_candidate_world->m_job_id == job_id and
                              m_candidate_world->m_world_generation == world_generation and
                              not m_candidate_world->m_candidate_uploads_complete
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, not uploads.empty() and uploads.size() <= 8u ? std::error_code{}
                                                                   : std::make_error_code(std::errc::invalid_argument));
        std::swap(m_world, *m_candidate_world);
        PPR_DEFER {
            std::swap(m_world, *m_candidate_world);
        };
        for (const auto &upload: uploads) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, appendDiagnostic_(upload));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, upload.m_cell_materials.size() == kCellsPerChunk
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            const auto applied = requestUpload(
                GridUploadRequest{
                    upload.m_chunk_id, std::span{&upload.m_payload, 1u},
                    std::span<const u16>{upload.m_cell_materials}});
            PPR_RETURN_ERROR_ON_FAIL(GridPass, applied);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, *applied == 1u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }
        if (final_batch) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, m_world.m_cell_cache.size() == m_world.m_submitted_tiles.size()
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            m_world.m_candidate_uploads_complete = true;
        }
        return {};
    }

    Expected<bool> GridPass::advanceWorldCandidate(
        rhi::IDevice &device, const FenceCompletionValue completed, const u32 chunk_budget) {
        reclaimCompleted_(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, m_candidate_world != nullptr ? std::error_code{}
                                                   : std::make_error_code(std::errc::operation_not_permitted));
        std::swap(m_world, *m_candidate_world);
        PPR_DEFER {
            std::swap(m_world, *m_candidate_world);
            m_resident_table_uploaded.fill(false);
            m_chunk_slot_uploaded.fill(false);
            m_visibility_prepared = false;
        };
        if (not m_world.m_candidate_uploads_complete) {
            const auto visuals = prepareTerrainVisuals_(device, 1u);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, visuals);
            return false;
        }
        const auto prepared = tryPrepareCellUploads(device, completed, chunk_budget);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, prepared);
        m_world.m_prepared = *prepared;
        return m_world.m_prepared;
    }

    std::error_code GridPass::stageDiagnostic_(const GridStagedFrame &stage) {
        if (stage.m_diagnostic_revision == 0u) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                stage.m_diagnostic_mode == EGridDiagnosticMode::textured and stage.m_diagnostic_palette.empty()
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            // Publish invalidation with the entire cell cohort, preserving the
            // old baseline while replacement slots are still being filled.
            m_world.m_initial_matter_invalidation_pending = m_world.m_initial_matter_invalidation_pending or
                                                            not stage.m_initial_matter_valid;
            return {}; // Ordinary terrain frames retain the active immutable preview.
        }
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_world.m_world_generation == 0u and m_world.m_diagnostic_revision == 0u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            stage.m_diagnostic_mode <= EGridDiagnosticMode::initial_matter and
                    stage.m_diagnostic_palette.size() <= 65536u and
                    (stage.m_diagnostic_mode == EGridDiagnosticMode::textured or not stage.m_diagnostic_palette.empty())
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            stage.m_diagnostic_mode != EGridDiagnosticMode::material or stage.m_diagnostic_palette.size() == 65536u
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));
        for (const auto &entry: stage.m_diagnostic_palette) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, std::isfinite(entry.m_fill_height_fine) and
                                  entry.m_fill_height_fine >= 0.0f and
                                  entry.m_fill_height_fine <= 4096.0f
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
        }
        m_world.m_diagnostic_mode = stage.m_diagnostic_mode;
        m_world.m_diagnostic_revision = stage.m_diagnostic_revision;
        m_world.m_diagnostic_palette = stage.m_diagnostic_palette;
        m_world.m_initial_matter_valid = stage.m_initial_matter_valid;
        return {};
    }

    std::error_code GridPass::appendDiagnostic_(const GridStagedUpload &upload) {
        if (m_world.m_diagnostic_mode == EGridDiagnosticMode::textured or
            m_world.m_diagnostic_mode == EGridDiagnosticMode::material) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, upload.m_diagnostic_indices.empty() ? std::error_code{}
                                                              : std::make_error_code(std::errc::invalid_argument));
            return {};
        }
        if (upload.m_diagnostic_indices.empty() and m_world.m_prepared) {
            return {}; // Later terrain edits retain immutable biome/temperature identity.
        }
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, upload.m_chunk_id < kGridTileCacheCapacity and
                              upload.m_diagnostic_indices.size() == kCellsPerChunk and
                              m_world.m_diagnostic_atlas == nullptr
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        auto &words = m_world.m_diagnostic_words[upload.m_chunk_id];
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, words.empty() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        words.resize(kCellWordsPerChunk);
        for (u32 index = 0u; index < kCellsPerChunk; index += 2u) {
            const u16 a = upload.m_diagnostic_indices[index], b = upload.m_diagnostic_indices[index + 1u];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, a < m_world.m_diagnostic_palette.size() and b < m_world.m_diagnostic_palette.size()
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            words[index / 2u] = a | (static_cast<u32>(b) << 16u);
        }
        return {};
    }

    Expected<bool> GridPass::prepareDiagnostic_(rhi::IDevice &device, const u32 chunk_budget) {
        if (m_world.m_diagnostic_mode == EGridDiagnosticMode::textured) {
            return true;
        }
        if (not m_world.m_diagnostic_colors) {
            rhi::BufferDesc desc{};
            desc.size = m_world.m_diagnostic_palette.size() * sizeof(GridDiagnosticColor);
            desc.elementSize = sizeof(GridDiagnosticColor);
            desc.usage = rhi::BufferUsage::ShaderResource;
            desc.defaultState = rhi::ResourceState::ShaderResource;
            desc.label = "grid diagnostic palette";
            // Initialization may enqueue an upload even before the bundle is drawn.
            m_world.m_upload_submission_pending = true;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass,
                device.createBuffer(desc, m_world.m_diagnostic_palette.data(), m_world.m_diagnostic_colors.writeRef()));
        }
        if (m_world.m_diagnostic_mode == EGridDiagnosticMode::material) {
            return true;
        }
        if (not m_world.m_diagnostic_atlas) {
            rhi::BufferDesc desc{};
            desc.size = static_cast<u64>(kGridTileCacheCapacity) * kCellWordsPerChunk * sizeof(u32);
            desc.elementSize = sizeof(u32);
            desc.memoryType = rhi::MemoryType::Upload;
            desc.usage = rhi::BufferUsage::ShaderResource;
            desc.defaultState = rhi::ResourceState::ShaderResource;
            desc.label = "grid diagnostic index atlas";
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, device.createBuffer(desc, nullptr, m_world.m_diagnostic_atlas.writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass,
                device.mapBuffer(m_world.m_diagnostic_atlas, rhi::CpuAccessMode::Write, &m_world.m_diagnostic_mapped));
        }
        auto *const target = static_cast<u32 *>(m_world.m_diagnostic_mapped);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, target != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        const u32 end = m_world.m_diagnostic_cursor +
                        std::min(chunk_budget, kGridTileCacheCapacity - m_world.m_diagnostic_cursor);
        for (; m_world.m_diagnostic_cursor < end; ++m_world.m_diagnostic_cursor) {
            const auto &words = m_world.m_diagnostic_words[m_world.m_diagnostic_cursor];
            u32 *const destination = target + m_world.m_diagnostic_cursor * kCellWordsPerChunk;
            if (words.empty()) {
                std::fill_n(destination, kCellWordsPerChunk, 0u);
            } else {
                std::memcpy(destination, words.data(), kCellWordsPerChunk * sizeof(u32));
            }
        }
        return m_world.m_diagnostic_cursor == kGridTileCacheCapacity;
    }

    std::error_code GridPass::activateWorldCandidate(
        const u64 job_id, const u64 world_generation, const u64 fingerprint) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_candidate_world and
                              m_candidate_world->m_prepared and
                              m_candidate_world->m_job_id == job_id and
                              m_candidate_world->m_world_generation == world_generation and
                              m_candidate_world->m_content_fingerprint == fingerprint
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));
        PPR_ASSERT(not m_retiring_world);
        std::swap(m_world, *m_candidate_world);
        m_retiring_world = std::move(m_candidate_world);
        m_resident_table_uploaded.fill(false);
        m_chunk_slot_uploaded.fill(false);
        m_visibility_prepared = false;
        return {};
    }

    void GridPass::cancelWorldCandidate(const u64 job_id) noexcept {
        if (m_candidate_world and m_candidate_world->m_job_id == job_id) {
            PPR_ASSERT(not m_retiring_world);
            // Even an unprepared header owns decoded images and chunk payloads.
            // Dispose it incrementally; submitted initialization also needs its fence.
            m_retiring_world = std::move(m_candidate_world);
        }
    }

    bool GridPass::worldCandidateAdmissionReady(const FenceCompletionValue completed) noexcept {
        reclaimCompleted_(completed);
        return not m_candidate_world and not m_retiring_world;
    }

    bool GridPass::worldCandidateMatches(
        const u64 job_id, const u64 world_generation, const u64 fingerprint) const noexcept {
        return m_candidate_world and
               m_candidate_world->m_job_id == job_id and
               m_candidate_world->m_world_generation == world_generation and
               m_candidate_world->m_content_fingerprint == fingerprint;
    }

    Expected<u32> GridPass::requestUpload(const GridUploadRequest &request) {
        m_visibility_prepared = false;
        // Pure CPU like submitTiles: the tile cache is pass-owned CPU data,
        // so quiescent tiles keep serving through device loss while
        // GPU-touching calls park.
        const GridTileSubmission *staged = nullptr;
        for (const GridTileSubmission &submission: m_world.m_submitted_tiles) {
            if (submission.m_chunk_id == request.m_chunk_id) {
                staged = &submission;
                break;
            }
        }
        if (staged == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        const u32 dirty_count = static_cast<u32>(std::popcount(staged->m_dirty_mask));
        const bool has_cells = not request.m_cell_materials.empty();
        if (request.m_tile_data_view.size() < dirty_count or
            (has_cells and request.m_cell_materials.size() != kCellsPerChunk)) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (has_cells) {
            const u32 chunk_x = request.m_chunk_id % (kGridWorldEdge / kGridTileEdge);
            const u32 chunk_y = request.m_chunk_id / (kGridWorldEdge / kGridTileEdge);
            const GridTileRange &range = staged->m_tile_range;
            if (request.m_chunk_id >= kGridTileCacheCapacity or
                range.m_min_x != static_cast<i32>(chunk_x * kGridTileEdge) or
                range.m_min_y != static_cast<i32>(chunk_y * kGridTileEdge) or
                range.m_max_x != static_cast<i32>((chunk_x + 1u) * kGridTileEdge) or
                range.m_max_y != static_cast<i32>((chunk_y + 1u) * kGridTileEdge)) [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
        }
        if (dirty_count == 0u) {
            // Quiescent chunk: nothing to upload, the cache keeps serving it.
            return 0u;
        }

        const bool is_new_entry = m_world.m_tile_cache.find(request.m_chunk_id) == m_world.m_tile_cache.end();
        if (is_new_entry and m_world.m_tile_cache.size() >= kGridTileCacheCapacity) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        // Pack before touching either cache. The low halfword is even cell i,
        // the high halfword is cell i+1 (including transparent 0xffff).
        std::unique_ptr<std::array<u32, kCellWordsPerChunk>> packed{};
        if (has_cells) {
            packed = std::make_unique<std::array<u32, kCellWordsPerChunk>>();
            for (u32 word = 0u; word < kCellWordsPerChunk; ++word) {
                (*packed)[word] = static_cast<u32>(request.m_cell_materials[word * 2u]) |
                                  (static_cast<u32>(request.m_cell_materials[word * 2u + 1u]) << 16u);
            }
        }

        // Chunk-sized tiles carry exactly one dirty tile (bit 0); the loop
        // stays general in tile-index order for a finer tiling that never
        // arrived, consuming one view element per set bit.
        u32 uploaded = 0u;
        for (u32 tile = 0u; tile < kTilesPerChunk; ++tile) {
            if ((staged->m_dirty_mask & (u64{1u} << tile)) == 0u) {
                continue;
            }
            GridCachedTile cached{};
            cached.m_payload = request.m_tile_data_view[uploaded];
            cached.m_material_id = staged->m_material_id;
            if (const auto found = m_world.m_tile_cache.find(request.m_chunk_id); found != m_world.m_tile_cache.end()) {
                found->second = cached;
            } else {
                m_world.m_tile_cache.emplace(request.m_chunk_id, cached);
            }
            ++uploaded;
        }

        m_upload_count += uploaded;
        m_world.m_resident_table_stale = true;
        if (has_cells) {
            const auto previous = m_world.m_cell_cache.find(request.m_chunk_id);
            // A deferred frame can replay the same unacknowledged revision.
            // Keep its partially copied cohort unless the actual cells change.
            if (previous == m_world.m_cell_cache.end() or *previous->second.m_words != *packed) {
                cancelCellCohort_();
                m_world.m_cell_cache.insert_or_assign(request.m_chunk_id, GridCachedCells{std::move(packed), true});
            }
        } else {
            cancelCellCohort_();
            m_world.m_cell_cache.erase(request.m_chunk_id);
            if (request.m_chunk_id < kGridTileCacheCapacity and m_world.m_cell_slots[request.m_chunk_id]) {
                m_world.m_cell_slots_current.reset(*m_world.m_cell_slots[request.m_chunk_id]);
                m_world.m_cell_slots[request.m_chunk_id].reset();
            }
        }
        return uploaded;
    }

    std::error_code GridPass::ensureCellBuffer_(rhi::IDevice &device, const bool atlas_required) {
        rhi::ComPtr<rhi::IBuffer> &buffer = atlas_required ? m_world.m_cell_atlas : m_world.m_cell_dummy;
        if (buffer != nullptr) {
            return default_value_v;
        }
        rhi::BufferDesc desc{};
        desc.size = atlas_required ? static_cast<u64>(kCellAtlasWords) * sizeof(u32) : sizeof(u32);
        desc.elementSize = sizeof(u32);
        desc.memoryType = rhi::MemoryType::Upload;
        desc.usage = rhi::BufferUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = atlas_required ? "grid cell material atlas" : "grid cell material dummy";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(desc, nullptr, buffer.writeRef()));
        if (atlas_required) {
            if (const std::error_code err = make_error_code(device.mapBuffer(
                    buffer.get(), rhi::CpuAccessMode::Write, &m_world.m_cell_atlas_mapped))) [[unlikely]] {
                buffer.setNull();
                return err;
            }
        }
        return default_value_v;
    }

    std::error_code GridPass::prepareCellUploads(
        rhi::IDevice &device, std23::function_ref<std::error_code()> wait_for_gpu_idle, const bool all_tiles) {
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        const auto visuals = prepareTerrainVisuals_(device, 256u);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, visuals);
        const auto diagnostic = prepareDiagnostic_(device, kGridTileCacheCapacity);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, diagnostic);
        Array<u32> pending{};
        const auto append_pending = [&](const u32 chunk_id) {
            const auto found = m_world.m_cell_cache.find(chunk_id);
            if (found != m_world.m_cell_cache.end() and
                found->second.m_pending and
                std::find(pending.begin(), pending.end(), chunk_id) == pending.end()) {
                pending.push_back(chunk_id);
            }
        };
        if (all_tiles) {
            for (const GridTileSubmission &tile: m_world.m_submitted_tiles) {
                append_pending(tile.m_chunk_id);
            }
        } else {
            const Expected<GridTilePlan> plan = planTiles(
                m_camera_view, std::span<const GridTileSubmission>{m_world.m_submitted_tiles}, m_world.m_cell_size_m);
            PPR_RETURN_ERROR_ON_FAIL(GridPass, plan ? std::error_code{} : plan.error());
            for (const GridPlannedTile &tile: plan->m_tiles) {
                append_pending(tile.m_chunk_id);
            }
        }
        if (pending.empty()) {
            m_world.m_cell_publication_deferred = not m_world.m_cell_cohort_chunks.empty();
            const bool pending_cells = std::ranges::any_of(
                m_world.m_cell_cache, [](const auto &entry) { return entry.second.m_pending; });
            if (not m_world.m_cell_publication_deferred and
                not pending_cells and
                m_world.m_initial_matter_invalidation_pending) {
                m_world.m_initial_matter_valid = false;
                m_world.m_initial_matter_invalidation_pending = false;
            }
            return default_value_v;
        }

        // Once a reference draw needs an upload, finish the whole pending
        // cohort. Publishing only visible replacements could pin both banks
        // and prevent a later interactive cohort from making progress.
        for (const auto entry: m_world.m_cell_cache) {
            append_pending(entry.first);
        }

        // Explicit reference fixtures retain their synchronous preparation API.
        // The interactive lane uses tryPrepareCellUploads instead.
        ++m_atlas_serialization_count;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_gpu_idle());
        const auto prepared = prepareCellCohort_(device, pending, m_last_submitted_value, kGridTileCacheCapacity);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, prepared);
        m_world.m_cell_publication_deferred = not *prepared;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, *prepared ? std::error_code{} : std::make_error_code(std::errc::no_buffer_space));
        const bool table_ready = std::ranges::all_of(m_world.m_submitted_tiles, [this](const GridTileSubmission &tile) {
            const auto found = m_world.m_cell_cache.find(tile.m_chunk_id);
            return found == m_world.m_cell_cache.end() or not found->second.m_pending;
        });
        if (table_ready and m_world.m_resident_table_stale) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, rebuildResidentTable_());
        }
        if (table_ready and m_world.m_initial_matter_invalidation_pending) {
            m_world.m_initial_matter_valid = false;
            m_world.m_initial_matter_invalidation_pending = false;
        }
        return {};
    }

    void GridPass::cancelCellCohort_() noexcept {
        for (const u32 slot: m_world.m_cell_cohort_slots) {
            m_world.m_cell_slots_reserved.reset(slot);
        }
        m_world.m_cell_cohort_chunks.clear();
        m_world.m_cell_cohort_slots.clear();
        m_world.m_cell_cohort_cursor = 0u;
    }

    Expected<bool> GridPass::tryPrepareCellUploads(
        rhi::IDevice &device, const FenceCompletionValue completed, const u32 chunk_budget) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, chunk_budget > 0u and chunk_budget <= kGridTileCacheCapacity
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        const auto visuals = prepareTerrainVisuals_(device, 1u);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, visuals);
        if (not *visuals) {
            m_world.m_cell_publication_deferred = true;
            return false;
        }
        Array<u32> pending;
        for (const auto entry: m_world.m_cell_cache) {
            if (entry.second.m_pending) {
                pending.push_back(entry.first);
            }
        }
        const auto prepared = prepareCellCohort_(device, pending, completed, chunk_budget);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, prepared);
        const auto diagnostic = prepareDiagnostic_(device, chunk_budget);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, diagnostic);
        if (*prepared and m_world.m_resident_table_stale) {
            // Retire the old table's pins before accepting another cohort,
            // even if frame admission later rejects the draw. Reader fences
            // continue protecting every retired physical slot.
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, rebuildResidentTable_());
        }
        m_world.m_cell_publication_deferred = not *prepared or not *diagnostic;
        if (m_world.m_cell_publication_deferred) {
            ++m_atlas_deferred_count;
        }
        if (*prepared and *diagnostic and m_world.m_initial_matter_invalidation_pending) {
            m_world.m_initial_matter_valid = false;
            m_world.m_initial_matter_invalidation_pending = false;
        }
        return *prepared and *diagnostic;
    }

    Expected<bool> GridPass::prepareCellCohort_(
        rhi::IDevice &device, const std::span<const u32> chunks, const FenceCompletionValue completed,
        const u32 chunk_budget) {
        if (chunks.empty()) {
            return true;
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, chunks.size() <= kGridTileCacheCapacity ? std::error_code{}
                                                              : std::make_error_code(std::errc::no_buffer_space));
        if (m_world.m_cell_cohort_chunks.empty()) {
            Array<u32> slots;
            slots.reserve(chunks.size());
            for (u32 slot = 0u; slot < kCellAtlasSlots and slots.size() < chunks.size(); ++slot) {
                if (not m_world.m_cell_slots_current.test(slot) and
                    not m_world.m_resident_cell_slots.test(slot) and
                    not m_world.m_cell_slots_reserved.test(slot) and
                    slotFenceComplete(completed, m_world.m_cell_slot_values[slot])) {
                    slots.push_back(slot);
                }
            }
            if (slots.size() != chunks.size()) {
                return false;
            }
            // Allocate/copy all CPU state before reserving any physical slot.
            Array<u32> cohort;
            cohort.assign(chunks.begin(), chunks.end());
            m_world.m_cell_cohort_chunks = std::move(cohort);
            m_world.m_cell_cohort_slots = std::move(slots);
            for (const u32 slot: m_world.m_cell_cohort_slots) {
                m_world.m_cell_slots_reserved.set(slot);
            }
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, ensureCellBuffer_(device, true));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, m_world.m_cell_atlas_mapped != nullptr ? std::error_code{}
                                                             : std::make_error_code(std::errc::invalid_argument));
        auto *const atlas = static_cast<u32 *>(m_world.m_cell_atlas_mapped);
        const u32 end = std::min<u32>(
            safe_narrowing(m_world.m_cell_cohort_chunks.size()), m_world.m_cell_cohort_cursor + chunk_budget);
        for (; m_world.m_cell_cohort_cursor < end; ++m_world.m_cell_cohort_cursor) {
            const u32 chunk_id = m_world.m_cell_cohort_chunks[m_world.m_cell_cohort_cursor];
            const u32 slot = m_world.m_cell_cohort_slots[m_world.m_cell_cohort_cursor];
            const auto found = m_world.m_cell_cache.find(chunk_id);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, found != m_world.m_cell_cache.end() and found->second.m_pending
                              ? std::error_code{}
                              : std::make_error_code(std::errc::state_not_recoverable));
            std::memcpy(
                atlas + static_cast<std::size_t>(slot) * kCellWordsPerChunk, found->second.m_words->data(),
                sizeof(u32) * kCellWordsPerChunk);
            m_atlas_uploaded_bytes += sizeof(u32) * kCellWordsPerChunk;
        }
        if (m_world.m_cell_cohort_cursor != m_world.m_cell_cohort_chunks.size()) {
            return false;
        }

        // Publish the complete mapping only after every chunk was copied.
        for (u32 index = 0u; index < m_world.m_cell_cohort_chunks.size(); ++index) {
            const u32 chunk = m_world.m_cell_cohort_chunks[index];
            const u32 slot = m_world.m_cell_cohort_slots[index];
            if (m_world.m_cell_slots[chunk]) {
                m_world.m_cell_slots_current.reset(*m_world.m_cell_slots[chunk]);
            }
            m_world.m_cell_slots[chunk] = slot;
            m_world.m_cell_slots_current.set(slot);
            m_world.m_cell_cache.find(chunk)->second.m_pending = false;
        }
        cancelCellCohort_();
        m_world.m_resident_table_stale = true;
        ++m_atlas_write_count;
        return true;
    }

    std::error_code GridPass::validateCellDraw_(const GridTilePlan &plan) const noexcept {
        for (const GridPlannedTile &tile: plan.m_tiles) {
            const auto found = m_world.m_cell_cache.find(tile.m_chunk_id);
            if (found != m_world.m_cell_cache.end() and (found->second.m_pending or m_world.m_cell_atlas == nullptr))
                [[unlikely]] {
                return std::make_error_code(std::errc::operation_in_progress);
            }
        }
        return default_value_v;
    }

    void GridPass::applyCellPayload_(GridTilePayload &payload, const u32 chunk_id) const noexcept {
        if (m_world.m_cell_cache.find(chunk_id) != m_world.m_cell_cache.end()) {
            PPR_ASSERT(m_world.m_cell_slots[chunk_id].has_value());
            payload.m_cell_word_base = *m_world.m_cell_slots[chunk_id] * kCellWordsPerChunk;
            payload.m_flags = 1u;
        } else {
            payload.m_cell_word_base = 0u;
            payload.m_flags = 0u;
        }
        payload.m_pad = 0u;
    }

    Expected<GridTilePayload> GridPass::resolveCached(const u32 chunk_id) noexcept {
        const auto found = m_world.m_tile_cache.find(chunk_id);
        if (found == m_world.m_tile_cache.end()) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        ++m_cache_hit_count;
        return found->second.m_payload;
    }

    void GridPass::notifyFrameSubmitted(const FenceCompletionValue submit_value) noexcept {
        m_last_submitted_value = submit_value;
        m_world.m_last_reader = submit_value;
        m_world.m_upload_submission_pending = false;
        for (WorldResources *const world: {m_candidate_world.get(), m_retiring_world.get()}) {
            if (world != nullptr and world->m_upload_submission_pending) {
                world->m_last_reader = submit_value;
                world->m_upload_submission_pending = false;
            }
        }
        m_tile_slot_values[m_last_used_slot] = submit_value;
        for (u32 slot = 0u; slot < kCellAtlasSlots; ++slot) {
            if (m_world.m_cell_slots_current.test(slot) or m_world.m_resident_cell_slots.test(slot)) {
                m_world.m_cell_slot_values[slot] = submit_value;
            }
        }
    }

    void GridPass::reclaimCompleted_(const FenceCompletionValue completed) noexcept {
        std::ignore = m_deferred_releases.reclaim(completed);
        if (m_retiring_world and
            not m_retiring_world->m_upload_submission_pending and
            m_retiring_world->m_last_reader <= completed and
            m_deferred_releases.pendingCount() == 0u) {
            // Shader bindings own resources too. Drop completed roots before
            // releasing the retired bundle or admitting another candidate.
            // Active roots with unfinished readers remain untouched.
            if (not m_retiring_world->m_retirement_roots_detached and m_cached_pipeline) {
                for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
                    if (slotFenceComplete(completed, m_tile_slot_values[slot])) {
                        m_cached_pipeline->m_slot_frame_cursors[slot] = {};
                        m_cached_pipeline->m_slot_roots[slot].setNull();
                        m_cached_pipeline->m_slot_atlases[slot] = nullptr;
                    }
                }
            }
            auto &retiring = *m_retiring_world;
            ++m_retirement_steps;
            u32 remaining_blocks = 8u;
            while (remaining_blocks != 0u and not retiring.m_cell_cache.empty()) {
                // Tail erasure never shifts the surviving flat-map payloads.
                retiring.m_cell_cache.erase(std::prev(retiring.m_cell_cache.end()));
                --remaining_blocks;
                ++m_retired_chunk_blocks;
            }
            while (remaining_blocks != 0u and retiring.m_retirement_diagnostic_cursor < kGridTileCacheCapacity) {
                auto &words = retiring.m_diagnostic_words[retiring.m_retirement_diagnostic_cursor++];
                if (words.capacity() != 0u) {
                    Array<u32>{}.swap(words);
                    --remaining_blocks;
                    ++m_retired_chunk_blocks;
                }
            }
            m_retiring_world->m_retirement_roots_detached = true;
            if (not retiring.m_texture_handles.empty()) {
                // Handles preserve upload/dedup refcounts until the last reader.
                PPR_VERIFY(not retiring.m_texture_cache->release(retiring.m_texture_handles.back()));
                retiring.m_texture_handles.pop_back();
            }
            if (not retiring.m_terrain_images.empty()) {
                retiring.m_terrain_images.pop_back();
            }
            if (retiring.m_cell_cache.empty() and
                retiring.m_retirement_diagnostic_cursor == kGridTileCacheCapacity and
                retiring.m_texture_handles.empty() and
                retiring.m_terrain_images.empty()) {
                // Only metadata and a fixed number of GPU objects remain.
                // Admission remains blocked until these owning releases finish.
                m_retiring_world.reset();
            }
        }
    }

    std::error_code GridPass::selectSlot_(const FenceCompletionValue completed, FenceWaitHook &wait_for_fence) {
        const std::span<const FenceCompletionValue> values{m_tile_slot_values.data(), m_tile_slot_values.size()};
        const u32 slot = selectRingSlot(completed, values, m_tile_ring_cursor);
        if (not slotFenceComplete(completed, m_tile_slot_values[slot])) [[unlikely]] {
            // Backstop only: 3 slots vs at most 2 outstanding submissions
            // means the scan above always finds a complete slot unless
            // completion is artificially delayed. Never overwrite early.
            PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_fence(m_tile_slot_values[slot]));
        }
        m_tile_ring_cursor = slot;
        m_last_used_slot = slot;
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // tiling encode
    // ------------------------------------------------------------------

    // Failure contract, phase by phase. This is NOT an atomic-frame guarantee:
    //   planning all-or-nothing. planTiles validates every staged submission
    //            before any encode, so one malformed range fails the whole call.
    //   upload   all-or-nothing. ensureTilePayloads_ grows the ring before any
    //            draw is encoded.
    //   encoding fail-closed single draw: the one instanced draw either encodes
    //            whole or the frame stays empty. A caller that needs
    //            whole-frame atomicity must discard the pass.
    std::error_code GridPass::render(const DrawContext &draw_context) {
        m_visibility_prepared = false;
        const hal::ProfileScope scope{"App.Pass.Grid"};
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        // Retire replaced pipelines/roots even on frames that encode
        // nothing: deferred objects must not outlive their completion.
        reclaimCompleted_(draw_context.m_completed_fence_value);
        if (m_world.m_submitted_tiles.empty()) {
            return default_value_v;
        }

        Expected<GridTilePlan> plan = planTiles(
            m_camera_view,
            std::span<const GridTileSubmission>{m_world.m_submitted_tiles.data(), m_world.m_submitted_tiles.size()},
            m_world.m_cell_size_m);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        if (plan->m_tiles.empty()) {
            return default_value_v;
        }
        PPR_RETURN_ERROR_ON_FAIL(GridPass, validateCellDraw_(*plan));
        const bool has_cells = std::ranges::any_of(plan->m_tiles, [this](const GridPlannedTile &tile) {
            return m_world.m_cell_cache.find(tile.m_chunk_id) != m_world.m_cell_cache.end();
        });
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, has_cells));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, false));

        // Resolve payloads: cache hits serve quiescent tiles, while a tile
        // with no cache entry yet stages cold straight from its submission so
        // the CPU-staged path still draws before its first upload.
        Array<GridTilePayload> payloads{};
        payloads.reserve(plan->m_tiles.size());
        for (const GridPlannedTile &tile: plan->m_tiles) {
            Expected<GridTilePayload> cached = resolveCached(tile.m_chunk_id);
            if (cached.has_value()) {
                applySubmissionBounds_(*cached, tile.m_tile_range);
                applyCellPayload_(*cached, tile.m_chunk_id);
                payloads.push_back(*cached);
                continue;
            }
            GridTilePayload staged_payload{};
            staged_payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            staged_payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            staged_payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            staged_payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            staged_payload.m_material = tile.m_material_id;
            applyCellPayload_(staged_payload, tile.m_chunk_id);
            payloads.push_back(staged_payload);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureTilePayloads_(
                          draw_context.m_device, safe_narrowing(payloads.size()), draw_context.m_completed_fence_value,
                          draw_context.m_wait_for_fence));

        // One rotation per submit onto a completion-selected slot: the draws
        // encoded after this point read the selected slot, whose last use
        // the fence has passed (or was waited out). The slot stays mapped
        // for its whole life, so there is no per-frame map/unmap pair. The
        // rotation is per render() submit and assumes a SINGLE
        // GridPass::render per frame (backpressured by acquireNextImage).
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, selectSlot_(draw_context.m_completed_fence_value, draw_context.m_wait_for_fence));
        void *const mapped_payloads = m_tile_payload_mapped[m_tile_ring_cursor];
        if (mapped_payloads == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        std::memcpy(
            mapped_payloads, payloads.data(), static_cast<std::size_t>(payloads.size()) * sizeof(GridTilePayload));
        // The reference lane shares storage with the persistent GPU table.
        m_resident_table_uploaded[m_tile_ring_cursor] = false;

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        rhi::IBuffer *const payload_buffer = m_tile_payloads[m_tile_ring_cursor].get();
        if (payload_buffer == nullptr or payload_buffer->getDesc().elementSize != sizeof(GridTilePayload))
            [[unlikely]] {
            PPR_LOG(
                GridPass, error, "tile payload stride mismatch vs shader expectation",
                {
                    {"payload_stride", payload_buffer == nullptr ? 0u : payload_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<GridCachedPipeline *> cached = pipelineFor_(draw_context.m_device, draw_context.m_render_pipeline_key);
        if (not cached.has_value()) [[unlikely]] {
            return cached.error();
        }
        rhi::IRenderPipeline *const pipeline = (*cached)->m_pipeline.get();
        rhi::IShaderObject *const root_object = (*cached)->m_slot_roots[m_tile_ring_cursor].get();
        rhi::ShaderCursor &frame_cursor = (*cached)->m_slot_frame_cursors[m_tile_ring_cursor];
        if (pipeline == nullptr or root_object == nullptr or not frame_cursor.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // Per-slot root object: bound as-is; this frame's slot cursor was
        // resolved at creation, so only the data below is written — and only
        // into a slot no in-flight submission still reads.
        draw_context.m_pass.bindPipeline(pipeline, root_object);
        rhi::ShaderCursor shader_cursor{root_object};

        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindSurfaceBuffers_(draw_context.m_device, shader_cursor, false));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_visible_ids"].setBinding(
                          rhi::Binding(m_world.m_cell_dummy.get(), makeFullRange(m_world.m_cell_dummy.get()))));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_tiles"].setBinding(rhi::Binding(payload_buffer, makeFullRange(payload_buffer))));
        rhi::IBuffer *const cell_buffer = has_cells ? m_world.m_cell_atlas.get() : m_world.m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            shader_cursor["g_cell_materials"].setBinding(rhi::Binding(cell_buffer, makeFullRange(cell_buffer))));
        m_cached_pipeline->m_slot_atlases[m_tile_ring_cursor] = cell_buffer == m_world.m_cell_atlas.get() ? cell_buffer
                                                                                                          : nullptr;

        draw_context.m_pass.draw({
            .vertexCount = 6u,
            .instanceCount = safe_narrowing(payloads.size()),
            .startInstanceLocation = 0u,
        });

        return default_value_v;
    }

    // Additive MDI lane beside render(): plan -> resolve payloads (cache hit,
    // or stage cold from the submission) -> upload tile ring plus the
    // compacted args ring -> one drawIndirect over the args. The CPU-staged
    // drawInstanced path above is untouched. Empty or fully-culled input
    // encodes zero draws and returns before any m_pass call.
    //
    // Offset discipline: payloads compact in plan order from slot 0 and
    // each arg carries its tile's
    // first-payload offset as startInstanceLocation instead. Whether the
    // shader receives that offset as SV_InstanceID is backend-dependent and
    // MUST be verified by an MDI GPU readback parity test; do not infer parity
    // from the CPU-side args alone.
    std::error_code GridPass::renderIndirect(const DrawContext &draw_context) {
        m_visibility_prepared = false;
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        reclaimCompleted_(draw_context.m_completed_fence_value);
        if (m_world.m_submitted_tiles.empty()) {
            return default_value_v;
        }

        Expected<GridTilePlan> plan = planTiles(
            m_camera_view,
            std::span<const GridTileSubmission>{m_world.m_submitted_tiles.data(), m_world.m_submitted_tiles.size()},
            m_world.m_cell_size_m);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        if (plan->m_tiles.empty()) {
            return default_value_v;
        }
        PPR_RETURN_ERROR_ON_FAIL(GridPass, validateCellDraw_(*plan));
        const bool has_cells = std::ranges::any_of(plan->m_tiles, [this](const GridPlannedTile &tile) {
            return m_world.m_cell_cache.find(tile.m_chunk_id) != m_world.m_cell_cache.end();
        });
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, has_cells));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, false));

        Expected<Array<rhi::IndirectDrawArguments>> args = compactIndirect(*plan);
        if (not args.has_value()) [[unlikely]] {
            return args.error();
        }

        // Same resolve rule as the CPU path: cache hits serve quiescent
        // tiles, while a tile with no cache entry yet stages cold straight
        // from its submission.
        Array<GridTilePayload> payloads{};
        payloads.reserve(plan->m_tiles.size());
        for (const GridPlannedTile &tile: plan->m_tiles) {
            Expected<GridTilePayload> cached = resolveCached(tile.m_chunk_id);
            if (cached.has_value()) {
                applySubmissionBounds_(*cached, tile.m_tile_range);
                applyCellPayload_(*cached, tile.m_chunk_id);
                payloads.push_back(*cached);
                continue;
            }
            GridTilePayload staged_payload{};
            staged_payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            staged_payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            staged_payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            staged_payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            staged_payload.m_material = tile.m_material_id;
            applyCellPayload_(staged_payload, tile.m_chunk_id);
            payloads.push_back(staged_payload);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureTilePayloads_(
                          draw_context.m_device, safe_narrowing(payloads.size()), draw_context.m_completed_fence_value,
                          draw_context.m_wait_for_fence));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureIndirectArgs_(
                          draw_context.m_device, safe_narrowing(args->size()), draw_context.m_completed_fence_value,
                          draw_context.m_wait_for_fence));

        // One rotation per submit, shared with the payload ring and covered
        // by the single completion array: the draws encoded after this point
        // read the selected slot in BOTH rings, which no in-flight frame is
        // still reading. Both slots stay mapped for their whole life, so
        // there is no per-frame map/unmap pair.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, selectSlot_(draw_context.m_completed_fence_value, draw_context.m_wait_for_fence));
        void *const mapped_payloads = m_tile_payload_mapped[m_tile_ring_cursor];
        void *const mapped_args = m_indirect_args_mapped[m_tile_ring_cursor];
        if (mapped_payloads == nullptr or mapped_args == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        std::memcpy(
            mapped_payloads, payloads.data(), static_cast<std::size_t>(payloads.size()) * sizeof(GridTilePayload));
        m_resident_table_uploaded[m_tile_ring_cursor] = false;
        std::memcpy(
            mapped_args, args->data(), static_cast<std::size_t>(args->size()) * sizeof(rhi::IndirectDrawArguments));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        rhi::IBuffer *const payload_buffer = m_tile_payloads[m_tile_ring_cursor].get();
        if (payload_buffer == nullptr or payload_buffer->getDesc().elementSize != sizeof(GridTilePayload))
            [[unlikely]] {
            PPR_LOG(
                GridPass, error, "tile payload stride mismatch vs shader expectation",
                {
                    {"payload_stride", payload_buffer == nullptr ? 0u : payload_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        rhi::IBuffer *const args_buffer = m_indirect_args[m_tile_ring_cursor].get();
        if (args_buffer == nullptr or args_buffer->getDesc().elementSize != sizeof(rhi::IndirectDrawArguments))
            [[unlikely]] {
            PPR_LOG(
                GridPass, error, "indirect args stride mismatch vs drawIndirect expectation",
                {
                    {"args_stride", args_buffer == nullptr ? 0u : args_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<GridCachedPipeline *> cached = pipelineFor_(draw_context.m_device, draw_context.m_render_pipeline_key);
        if (not cached.has_value()) [[unlikely]] {
            return cached.error();
        }
        rhi::IRenderPipeline *const pipeline = (*cached)->m_pipeline.get();
        rhi::IShaderObject *const root_object = (*cached)->m_slot_roots[m_tile_ring_cursor].get();
        rhi::ShaderCursor &frame_cursor = (*cached)->m_slot_frame_cursors[m_tile_ring_cursor];
        if (pipeline == nullptr or root_object == nullptr or not frame_cursor.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // Same bindings as the CPU path: this frame's slot root rebound
        // as-is, the frame constants (per-draw bases ride the
        // args, see above), and the payload buffer.
        draw_context.m_pass.bindPipeline(pipeline, root_object);
        rhi::ShaderCursor shader_cursor{root_object};

        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindSurfaceBuffers_(draw_context.m_device, shader_cursor, false));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_visible_ids"].setBinding(
                          rhi::Binding(m_world.m_cell_dummy.get(), makeFullRange(m_world.m_cell_dummy.get()))));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_tiles"].setBinding(rhi::Binding(payload_buffer, makeFullRange(payload_buffer))));
        rhi::IBuffer *const cell_buffer = has_cells ? m_world.m_cell_atlas.get() : m_world.m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            shader_cursor["g_cell_materials"].setBinding(rhi::Binding(cell_buffer, makeFullRange(cell_buffer))));
        m_cached_pipeline->m_slot_atlases[m_tile_ring_cursor] = cell_buffer == m_world.m_cell_atlas.get() ? cell_buffer
                                                                                                          : nullptr;

        draw_context.m_pass.drawIndirect(safe_narrowing(args->size()), rhi::BufferOffsetPair{args_buffer});

        return default_value_v;
    }

    std::error_code GridPass::ensureTilePayloads_(
        rhi::IDevice &device, const u32 tile_count, const FenceCompletionValue completed,
        FenceWaitHook &wait_for_fence) {
        if (tile_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 payload_bytes = static_cast<u64>(tile_count) * sizeof(GridTilePayload);
        if (m_tile_payloads[0] != nullptr and m_tile_payload_capacity >= payload_bytes) {
            return default_value_v;
        }

        // Growth or first use: the whole ring is replaced, so every
        // submission that may still read it must complete first. Waiting for
        // the youngest tracked use covers all three slots at once; the wait
        // is skipped when nothing was ever submitted, or when the frame's
        // polled completion already passes every tracked use. Growth MUST
        // wait — unfenced replacement here was the highest-severity reuse
        // hole (R6).
        {
            const std::span<const FenceCompletionValue> values{m_tile_slot_values.data(), m_tile_slot_values.size()};
            const FenceCompletionValue youngest = maxSlotValue(values);
            if (youngest != kFenceValueNone and youngest > completed) {
                ++m_growth_wait_count;
                PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_fence(youngest));
            }
        }

        // Drop the whole ring (and unmap it, since a device is still
        // available here) before rebuilding it at the new size. The wait
        // above drained every tracked submission, so the shared completion
        // array restarts empty alongside the fresh buffers.
        releaseTilePayloads_(std::addressof(device));
        m_tile_slot_values.fill(kFenceValueNone);

        rhi::BufferDesc payload_desc{};
        payload_desc.size = payload_bytes;
        payload_desc.elementSize = sizeof(GridTilePayload);
        payload_desc.memoryType = rhi::MemoryType::Upload;
        payload_desc.usage = rhi::BufferUsage::ShaderResource;
        payload_desc.defaultState = rhi::ResourceState::ShaderResource;
        payload_desc.label = "grid tile payloads";
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (const std::error_code err = make_error_code(
                    device.createBuffer(payload_desc, nullptr, m_tile_payloads[slot].writeRef()))) [[unlikely]] {
                releaseTilePayloads_(std::addressof(device));
                return err;
            }
            if (const std::error_code err = make_error_code(device.mapBuffer(
                    m_tile_payloads[slot].get(), rhi::CpuAccessMode::Write,
                    std::addressof(m_tile_payload_mapped[slot])))) [[unlikely]] {
                releaseTilePayloads_(std::addressof(device));
                return err;
            }
        }

        m_tile_payload_capacity = payload_bytes;
        return default_value_v;
    }

    void GridPass::releaseTilePayloads_(rhi::IDevice *const device) noexcept {
        m_resident_table_uploaded.fill(false);
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (m_tile_payloads[slot] == nullptr) {
                m_tile_payload_mapped[slot] = nullptr;
                continue;
            }

            // Unmap only when a device survived: shutdown and device loss have
            // none to ask, and leaving those buffers mapped is safe because
            // SLANG_RHI_DEBUG_ENABLE_BUFFER_MAP_VALIDATION defaults to 0, so a
            // still-mapped release does not poison the debug layer.
            if (device != nullptr) {
                std::ignore = device->unmapBuffer(m_tile_payloads[slot].get());
            }
            m_tile_payloads[slot].setNull();
            m_tile_payload_mapped[slot] = nullptr;
        }
        m_tile_payload_capacity = 0u;
        m_tile_ring_cursor = 0u;
    }

    std::error_code GridPass::ensureIndirectArgs_(
        rhi::IDevice &device, const u32 arg_count, const FenceCompletionValue completed,
        FenceWaitHook &wait_for_fence) {
        if (arg_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 args_bytes = static_cast<u64>(arg_count) * sizeof(rhi::IndirectDrawArguments);
        if (m_indirect_args[0] != nullptr and m_indirect_args_capacity >= args_bytes) {
            return default_value_v;
        }

        // Growth or first use: same whole-ring drop as the payload ring (and
        // unmap it while a device is still available here). The shared
        // completion array governs both rings, so the youngest-use wait
        // below covers the args ring together with the payload ring.
        {
            const std::span<const FenceCompletionValue> values{m_tile_slot_values.data(), m_tile_slot_values.size()};
            const FenceCompletionValue youngest = maxSlotValue(values);
            if (youngest != kFenceValueNone and youngest > completed) {
                ++m_growth_wait_count;
                PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_fence(youngest));
            }
        }
        releaseIndirectArgs_(std::addressof(device));
        m_tile_slot_values.fill(kFenceValueNone);

        rhi::BufferDesc args_desc{};
        args_desc.size = args_bytes;
        args_desc.elementSize = sizeof(rhi::IndirectDrawArguments);
        args_desc.memoryType = rhi::MemoryType::Upload;
        args_desc.usage = rhi::BufferUsage::IndirectArgument;
        args_desc.defaultState = rhi::ResourceState::IndirectArgument;
        args_desc.label = "grid indirect args";
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (const std::error_code err = make_error_code(
                    device.createBuffer(args_desc, nullptr, m_indirect_args[slot].writeRef()))) [[unlikely]] {
                releaseIndirectArgs_(std::addressof(device));
                return err;
            }
            if (const std::error_code err = make_error_code(device.mapBuffer(
                    m_indirect_args[slot].get(), rhi::CpuAccessMode::Write,
                    std::addressof(m_indirect_args_mapped[slot])))) [[unlikely]] {
                releaseIndirectArgs_(std::addressof(device));
                return err;
            }
        }

        m_indirect_args_capacity = args_bytes;
        return default_value_v;
    }

    void GridPass::releaseIndirectArgs_(rhi::IDevice *const device) noexcept {
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (m_indirect_args[slot] == nullptr) {
                m_indirect_args_mapped[slot] = nullptr;
                continue;
            }

            // Same unmap contract as the payload ring: only when a device
            // survived (growth/replacement); shutdown and device loss release
            // still-mapped buffers as-is.
            if (device != nullptr) {
                std::ignore = device->unmapBuffer(m_indirect_args[slot].get());
            }
            m_indirect_args[slot].setNull();
            m_indirect_args_mapped[slot] = nullptr;
        }
        m_indirect_args_capacity = 0u;
    }

    Expected<GridPass::GridCachedPipeline *> GridPass::pipelineFor_(
        rhi::IDevice &device, const RenderPipelineSignature &signature) {
        if (m_shader_program == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        // Shape validation runs BEFORE the cache is touched: an unsupported
        // signature must never drop the pipeline built for the last good one.
        // The sample count is a SampleCount by construction — all four
        // enumerators are accepted — so only the color shape is checked.
        if (signature.colorFormats().size() != 1u) {
            PPR_LOG(
                GridPass, error, "unsupported render pipeline signature",
                {
                    {"color_format_count", signature.colorFormats().size()},
                    {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                    {"sample_count", static_cast<u32>(signature.m_sample_count)},
                });
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        if (not m_render_pipeline_key.has_value() or
            static_cast<const RenderPipelineSignature &>(m_render_pipeline_key.value()) != signature) {
            PPR_LOG(GridPass, debug, "pipeline cache cleared on signature change");
            // Deferred release, not an immediate drop: in-flight submissions
            // may still reference the old pipeline and its roots. The entry
            // retires once the fence passes the latest submission that could
            // reference it; reclaim runs at every render entry.
            if (m_cached_pipeline.has_value()) {
                GridCachedPipeline retired = std::move(*m_cached_pipeline);
                m_cached_pipeline.reset();
                const FenceCompletionValue last_use = m_last_submitted_value;
                m_deferred_releases.defer(last_use, [held = std::move(retired)]() mutable {
                    held.m_pipeline.setNull();
                    for (rhi::ComPtr<rhi::IShaderObject> &root: held.m_slot_roots) {
                        root.setNull();
                    }
                });
            }
            m_render_pipeline_key.reset();
        }

        if (m_cached_pipeline.has_value()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, ensurePipelineRoots_(device, *m_cached_pipeline));
            return &m_cached_pipeline.value();
        }

        rhi::ColorTargetDesc color_target{};
        color_target.format = signature.colorFormats().front();
        color_target.enableBlend = true;
        color_target.color.srcFactor = rhi::BlendFactor::SrcAlpha;
        color_target.color.dstFactor = rhi::BlendFactor::InvSrcAlpha;
        color_target.color.op = rhi::BlendOp::Add;
        color_target.alpha.srcFactor = rhi::BlendFactor::One;
        color_target.alpha.dstFactor = rhi::BlendFactor::InvSrcAlpha;
        color_target.alpha.op = rhi::BlendOp::Add;

        rhi::RenderPipelineDesc pipeline_desc{};
        pipeline_desc.program = m_shader_program.get();
        pipeline_desc.inputLayout = nullptr;
        pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        pipeline_desc.targets = &color_target;
        pipeline_desc.targetCount = 1u;
        pipeline_desc.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
        pipeline_desc.depthStencil.format = signature.m_depth_stencil_format.value_or(rhi::Format::Undefined);
        pipeline_desc.depthStencil.depthTestEnable = signature.m_depth_stencil_format.has_value();
        pipeline_desc.depthStencil.depthWriteEnable = signature.m_depth_stencil_format.has_value();
        pipeline_desc.depthStencil.depthFunc = rhi::ComparisonFunc::LessEqual;
        // Tiles are flat quads on the z = 0 plane: culling is off so the winding
        // choice in the vertex entry can never hide a tile.
        pipeline_desc.rasterizer.cullMode = rhi::CullMode::None;
        pipeline_desc.label = "grid tiles pipeline";

        rhi::ComPtr<rhi::IRenderPipeline> pipeline{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, device.createRenderPipeline(pipeline_desc, pipeline.writeRef()));

        m_render_pipeline_key.emplace(signature);
        m_cached_pipeline.emplace();
        m_cached_pipeline->m_pipeline = std::move(pipeline);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, ensurePipelineRoots_(device, *m_cached_pipeline));

        return &m_cached_pipeline.value();
    }

    std::error_code GridPass::ensurePipelineRoots_(rhi::IDevice &device, GridCachedPipeline &pipeline) {
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (pipeline.m_slot_roots[slot]) {
                continue;
            }
            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createRootShaderObject(pipeline.m_pipeline->getProgram(), root.writeRef()));
            const rhi::ShaderCursor cursor{root.get()};
            const rhi::ShaderCursor field = cursor["g_grid_frame"];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, field.isValid() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            rhi::ShaderCursor frame;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, field.getDereferenced(frame));
            pipeline.m_slot_frame_cursors[slot] = frame;
            pipeline.m_slot_roots[slot] = std::move(root);
        }
        return {};
    }

    GridPassObservations GridPass::observations() const noexcept {
        GridPassObservations out{
            .m_draw_commands = m_draw_commands,
            .m_dispatch_commands = m_dispatch_commands,
            .m_atlas_uploaded_bytes = m_atlas_uploaded_bytes,
            .m_atlas_writes = m_atlas_write_count,
            .m_atlas_serializations = m_atlas_serialization_count,
            .m_atlas_deferrals = m_atlas_deferred_count,
            .m_growth_waits = m_growth_wait_count,
            .m_table_builds = m_resident_table_build_count,
            .m_table_uploads = m_resident_table_upload_count,
            .m_retirement_steps = m_retirement_steps,
            .m_retired_chunk_blocks = m_retired_chunk_blocks,
            .m_retained_atlases = retainedAtlasCountForTest(),
        };
        const auto count_textures = [&](const WorldResources &world) {
            if (world.m_texture_cache) {
                out.m_texture_entries += world.m_texture_cache->entryCount();
                out.m_texture_slots += world.m_texture_cache->textureUsed();
            }
        };
        count_textures(m_world);
        if (m_candidate_world) {
            count_textures(*m_candidate_world);
        }
        if (m_retiring_world) {
            count_textures(*m_retiring_world);
        }
        return out;
    }

    u32 GridPass::retainedAtlasCountForTest() const noexcept {
        std::array<const rhi::IBuffer *, 6u> atlases{};
        u32 count = 0u;
        const auto append = [&](const rhi::IBuffer *const atlas) {
            if (atlas != nullptr and
                std::find(atlases.begin(), atlases.begin() + count, atlas) == atlases.begin() + count) {
                atlases[count++] = atlas;
            }
        };
        append(m_world.m_cell_atlas.get());
        append(m_candidate_world ? m_candidate_world->m_cell_atlas.get() : nullptr);
        append(m_retiring_world ? m_retiring_world->m_cell_atlas.get() : nullptr);
        if (m_cached_pipeline) {
            for (const auto *const atlas: m_cached_pipeline->m_slot_atlases) {
                append(atlas);
            }
        }
        return count;
    }

    // ------------------------------------------------------------------
    // teardown and resource initialization
    // ------------------------------------------------------------------

    std::error_code GridPass::shutdown() {
        PPR_LOG(
            GridPass, info, "GridPass shut down",
            {
                {"staged_tiles", m_world.m_submitted_tiles.size()},
                {"cached_tiles", m_world.m_tile_cache.size()},
            });

        // Teardown order: stop submissions first (no new work), then drop the
        // cached pipeline roots (their bindings reference the payload ring),
        // then the CPU tile cache, then the pass's own program and buffers.
        // Precondition (established by the Editor): GPU completion was
        // already proven idle, so no pending frame can read what is
        // released here. Deferred releases drain unconditionally — after
        // confirmed loss the objects are dead either way. Slot values reset
        // alongside the fence epoch they named. Retain-first-error,
        // best-effort.
        std::error_code first_err{};
        m_candidate_world.reset();
        m_retiring_world.reset();
        m_world.m_submitted_tiles.clear();
        clearCache();
        m_upload_count = 0u;
        m_cache_hit_count = 0u;
        m_atlas_serialization_count = 0u;
        m_atlas_write_count = 0u;
        m_world.m_cell_size_m = 1.0f;
        m_world.m_contours_enabled = false;
        m_world.m_resident_contours_enabled = false;
        m_world.m_resident_cell_size_m = 1.0f;
        m_world.m_boundary_material_id = 1u;
        m_world.m_resident_boundary_material_id = 1u;
        m_world.m_job_id = 0u;
        m_world.m_world_generation = 0u;
        m_world.m_content_fingerprint = 0u;
        m_world.m_visuals_set = false;
        m_world.m_terrain_visuals.clear();
        m_world.m_terrain_images.clear();
        m_world.m_diagnostic_mode = EGridDiagnosticMode::textured;
        m_world.m_diagnostic_revision = 0u;
        m_world.m_diagnostic_palette.clear();
        m_world.m_diagnostic_words = {};
        m_world.m_diagnostic_cursor = 0u;
        m_world.m_initial_matter_valid = true;
        m_world.m_initial_matter_invalidation_pending = false;
        m_world.m_diagnostic_atlas.setNull();
        m_world.m_diagnostic_mapped = nullptr;
        m_world.m_diagnostic_colors.setNull();
        m_world.m_prepared = false;
        m_world.m_last_reader = kFenceValueNone;
        m_world.m_upload_submission_pending = false;
        m_world.m_cell_slot_values.fill(kFenceValueNone);
        m_world.m_resident_cell_slots.reset();
        m_atlas_deferred_count = 0u;
        m_atlas_uploaded_bytes = 0u;
        m_world.m_resident_table.clear();
        m_draw_commands = 0u;
        m_dispatch_commands = 0u;
        m_retirement_steps = 0u;
        m_retired_chunk_blocks = 0u;
        m_resident_table_build_count = 0u;
        m_resident_table_upload_count = 0u;
        m_growth_wait_count = 0u;
        std::ignore = m_deferred_releases.drainAll();
        m_last_submitted_value = kFenceValueNone;
        m_tile_slot_values.fill(kFenceValueNone);
        m_last_used_slot = 0u;
        m_cached_pipeline.reset();
        m_render_pipeline_key.reset();
        m_world.m_terrain_palette.setNull();
        if (m_world.m_texture_cache) {
            PPR_RETAIN_ERROR_ON_FAIL(GridPass, first_err, m_world.m_texture_cache->shutdown());
            m_world.m_texture_cache.reset();
        }
        m_world.m_texture_slots.clear();
        m_world.m_texture_handles.clear();
        m_world.m_image_cursor = 0u;
        m_terrain_sampler.setNull();
        m_terrain_fallback_view.setNull();
        m_terrain_fallback.setNull();
        m_terrain_fallback_handle = {};
        m_terrain_sampler_handle = {};
        m_ready = false;

        m_shader_program.setNull();
        m_visibility_programs = {};
        m_visibility_slots = {};
        m_visibility_prepared = false;
        m_world.m_cell_atlas.setNull();
        m_world.m_cell_atlas_mapped = nullptr;
        m_world.m_cell_dummy.setNull();
        m_contour_points.setNull();
        m_contour_cases.setNull();
        m_chunk_slot_buffers = {};
        m_chunk_slot_mapped = {};
        m_chunk_slot_uploaded.fill(false);
        releaseIndirectArgs_(nullptr);
        releaseTilePayloads_(nullptr);
        return first_err;
    }

    std::error_code GridPass::notifyDeviceLost() noexcept {
        m_world.m_diagnostic_atlas.setNull();
        m_world.m_diagnostic_colors.setNull();
        m_world.m_diagnostic_mapped = nullptr;
        m_world.m_diagnostic_cursor = 0u;
        // Same order as shutdown, retain-first-error. CPU records (staged
        // submissions, tile cache, counters) are KEPT: resolveCached still
        // serves quiescent tiles while GPU-touching calls park. Programs
        // survive device loss, cached pipelines do not. Deferred releases
        // are discarded outright — the GPU objects are gone, so nothing is
        // in flight worth waiting for (same contract as the Renderer and the
        // pass-owned caches). Slot values reset with the fence epoch.
        std::error_code first_err{};
        m_candidate_world.reset();
        m_retiring_world.reset();
        m_world.m_submitted_tiles.clear();
        m_world.m_resident_table_stale = true;
        cancelCellCohort_();
        m_world.m_cell_slots.fill(std::nullopt);
        m_world.m_cell_slots_current.reset();
        m_world.m_resident_cell_slots.reset();
        m_world.m_cell_slot_values.fill(kFenceValueNone);
        m_world.m_cell_publication_deferred = false;
        m_world.m_last_reader = kFenceValueNone;
        m_world.m_upload_submission_pending = false;
        m_world.m_prepared = false;
        std::ignore = m_deferred_releases.drainAll();
        m_last_submitted_value = kFenceValueNone;
        m_tile_slot_values.fill(kFenceValueNone);
        m_last_used_slot = 0u;
        m_cached_pipeline.reset();
        m_render_pipeline_key.reset();
        m_world.m_terrain_palette.setNull();
        if (m_world.m_texture_cache) {
            PPR_RETAIN_ERROR_ON_FAIL(GridPass, first_err, m_world.m_texture_cache->shutdown());
            m_world.m_texture_cache.reset();
        }
        m_world.m_texture_slots.clear();
        m_world.m_texture_handles.clear();
        m_world.m_image_cursor = 0u;
        m_terrain_sampler.setNull();
        m_terrain_fallback_view.setNull();
        m_terrain_fallback.setNull();
        m_terrain_fallback_handle = {};
        m_terrain_sampler_handle = {};
        m_world.m_cell_atlas.setNull();
        m_visibility_programs = {};
        m_visibility_slots = {};
        m_visibility_prepared = false;
        m_world.m_cell_atlas_mapped = nullptr;
        m_world.m_cell_dummy.setNull();
        m_contour_points.setNull();
        m_contour_cases.setNull();
        m_chunk_slot_buffers = {};
        m_chunk_slot_mapped = {};
        m_chunk_slot_uploaded.fill(false);
        for (auto it = m_world.m_cell_cache.begin(); it != m_world.m_cell_cache.end(); ++it) {
            it->second.m_pending = true;
        }
        releaseIndirectArgs_(nullptr);
        releaseTilePayloads_(nullptr);
        if (not first_err) {
            m_ready = false;
        }
        return first_err;
    }

    std::error_code GridPass::createShaderProgram_(
        IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule grid_shader{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            shader_service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("grid_tiles.slang"), "grid_tiles", grid_shader.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, grid_shader->findEntryPointByName("vertexGridMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, grid_shader->findEntryPointByName("fragmentGridMain", fragment_ep.writeRef()));

        // One zero-based payload table. The GPU lane resolves visible IDs;
        // reference draws use their instance IDs directly.
        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = grid_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

        return default_value_v;
    }

    std::error_code GridPass::createVisibilityPrograms_(
        IShaderService &service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("grid_visibility.slang"), "grid_visibility", module.writeRef()));
        std::array<VisibilityProgram, 2u> programs{};
        const std::array<const char *, 2u> names{"resetVisibility", "cullVisibility"};
        for (u32 index = 0u; index < programs.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(names[index], entry.writeRef()));
            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            VisibilityProgram &program = programs[index];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createShaderProgram(desc, program.m_program.writeRef(), diagnostics.writeRef()));
            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = program.m_program.get();
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createComputePipeline(pipeline, program.m_pipeline.writeRef()));
            for (auto &root: program.m_roots) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, device.createRootShaderObject(program.m_program.get(), root.writeRef()));
            }
        }
        m_visibility_programs = std::move(programs);
        return {};
    }

    std::error_code GridPass::ensureVisibilityBuffers_(rhi::IDevice &device) {
        if (m_visibility_slots[0].m_ids) {
            return {};
        }
        std::array<VisibilitySlot, kTileRingSize> slots{};
        for (VisibilitySlot &slot: slots) {
            rhi::BufferDesc ids{};
            ids.size = kGridTileCacheCapacity * sizeof(u32);
            ids.elementSize = sizeof(u32);
            ids.memoryType = rhi::MemoryType::DeviceLocal;
            ids.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            ids.defaultState = rhi::ResourceState::UnorderedAccess;
            ids.label = "grid GPU visible IDs";
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(ids, nullptr, slot.m_ids.writeRef()));
            rhi::BufferDesc args = ids;
            args.size = sizeof(rhi::IndirectDrawArguments);
            args.elementSize = sizeof(rhi::IndirectDrawArguments);
            args.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::IndirectArgument) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            args.label = "grid GPU draw arguments";
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(args, nullptr, slot.m_args.writeRef()));
        }
        m_visibility_slots = std::move(slots);
        return {};
    }

    std::error_code GridPass::ensureSurfaceBuffers_(rhi::IDevice &device) {
        if (m_contour_cases) {
            return {};
        }
        static_assert(sizeof(math::ContourPoint) == 8u);
        static_assert(sizeof(math::ContourTriangle) == 16u);
        static_assert(sizeof(math::ContourCase) == 72u);
        static_assert(PPR_OFFSETOF(math::ContourCase, m_segments) == 8u);
        static_assert(PPR_OFFSETOF(math::ContourCase, m_triangles) == 24u);
        rhi::ComPtr<rhi::IBuffer> points;
        rhi::ComPtr<rhi::IBuffer> cases;
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> maps{};
        std::array<void *, kTileRingSize> mapped{};
        rhi::BufferDesc desc{};
        desc.memoryType = rhi::MemoryType::Upload;
        desc.usage = rhi::BufferUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.elementSize = sizeof(math::ContourPoint);
        desc.size = math::contourPoints().size_bytes();
        desc.label = "grid shared contour points";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(desc, math::contourPoints().data(), points.writeRef()));
        desc.elementSize = sizeof(math::ContourCase);
        desc.size = math::contourCases().size_bytes();
        desc.label = "grid shared contour cases";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(desc, math::contourCases().data(), cases.writeRef()));
        desc.elementSize = sizeof(u32);
        desc.size = sizeof(u32) * kGridTileCacheCapacity;
        desc.label = "grid logical chunk slots";
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(desc, nullptr, maps[slot].writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.mapBuffer(maps[slot], rhi::CpuAccessMode::Write, &mapped[slot]));
        }
        m_contour_points = std::move(points);
        m_contour_cases = std::move(cases);
        m_chunk_slot_buffers = std::move(maps);
        m_chunk_slot_mapped = mapped;
        return {};
    }

    std::error_code GridPass::ensureTerrainSampler_(rhi::IDevice &device) {
        if (m_terrain_sampler != nullptr) {
            return {};
        }
        constexpr u32 white = 0xffffffffu;
        const rhi::SubresourceData initial{.data = &white, .rowPitch = 4u, .slicePitch = 4u};
        rhi::TextureDesc desc{};
        desc.type = rhi::TextureType::Texture2D;
        desc.size = {.width = 1u, .height = 1u, .depth = 1u};
        desc.arrayLength = 1u;
        desc.mipCount = 1u;
        desc.format = rhi::Format::RGBA8Unorm;
        desc.memoryType = rhi::MemoryType::DeviceLocal;
        desc.usage = rhi::TextureUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "terrain fallback white";
        rhi::ComPtr<rhi::ITexture> texture;
        rhi::ComPtr<rhi::ITextureView> view;
        rhi::ComPtr<rhi::ISampler> sampler;
        rhi::DescriptorHandle fallback_handle{}, sampler_handle{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, &initial, texture.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, texture->getDefaultView(view.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, view->getDescriptorHandle(rhi::DescriptorHandleAccess::Read, &fallback_handle));
        rhi::SamplerDesc sampler_desc{};
        sampler_desc.minFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.magFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.mipFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.addressU = rhi::TextureAddressingMode::Wrap;
        sampler_desc.addressV = rhi::TextureAddressingMode::Wrap;
        sampler_desc.addressW = rhi::TextureAddressingMode::Wrap;
        sampler_desc.maxAnisotropy = 1u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createSampler(sampler_desc, sampler.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, sampler->getDescriptorHandle(&sampler_handle));
        m_terrain_fallback = std::move(texture);
        m_terrain_fallback_view = std::move(view);
        m_terrain_sampler = std::move(sampler);
        m_terrain_fallback_handle = fallback_handle;
        m_terrain_sampler_handle = sampler_handle;
        return {};
    }

    Expected<bool> GridPass::prepareTerrainVisuals_(rhi::IDevice &device, const u32 image_budget) {
        if (m_world.m_terrain_palette != nullptr) {
            return true;
        }
        m_world.m_upload_submission_pending = true;
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, ensureTerrainSampler_(device));
        if (not m_world.m_texture_cache) {
            auto cache = std::make_unique<BindlessTextureCache>();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, cache->initialize(
                              device, safe_narrowing(m_world.m_terrain_images.size() + 1u), m_terrain_fallback_handle));
            m_world.m_texture_slots.resize(m_world.m_terrain_images.size());
            m_world.m_texture_cache = std::move(cache);
        }
        const u32 end = std::min<u32>(
            safe_narrowing(m_world.m_terrain_images.size()), m_world.m_image_cursor + image_budget);
        for (; m_world.m_image_cursor < end; ++m_world.m_image_cursor) {
            const auto texture = m_world.m_texture_cache->upload(m_world.m_terrain_images[m_world.m_image_cursor]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, texture);
            const auto slot = m_world.m_texture_cache->residentIndex(*texture);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, slot);
            m_world.m_texture_handles.push_back(*texture);
            m_world.m_texture_slots[m_world.m_image_cursor] = slot->m_value;
        }
        if (m_world.m_image_cursor != m_world.m_terrain_images.size()) {
            return false;
        }
        // One immutable dense lookup per world. Ordinary SRV indexing is
        // separate from the nonuniform descriptor lookup in the fragment shader.
        Array<GridTerrainVisual> palette;
        palette.resize(m_world.m_terrain_visuals.empty() ? 1u : 65536u);
        for (GridTerrainVisual visual: m_world.m_terrain_visuals) {
            visual.m_texture_index = visual.m_texture_index == 0xffffffffu
                                         ? 0u
                                         : m_world.m_texture_slots[visual.m_texture_index];
            palette[visual.m_material_id] = visual;
        }
        rhi::BufferDesc desc{};
        desc.size = palette.size() * sizeof(GridTerrainVisual);
        desc.elementSize = sizeof(GridTerrainVisual);
        desc.memoryType = rhi::MemoryType::Upload;
        desc.usage = rhi::BufferUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "terrain material palette";
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, device.createBuffer(desc, palette.data(), m_world.m_terrain_palette.writeRef()));
        return true;
    }

    std::error_code GridPass::bindSurfaceBuffers_(
        rhi::IDevice &device, rhi::ShaderCursor &cursor, const bool retained_table) {
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureSurfaceBuffers_(device));
        const auto visuals = prepareTerrainVisuals_(device, 0u);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, visuals);
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, *visuals ? std::error_code{} : std::make_error_code(std::errc::operation_in_progress));
        rhi::IBuffer *const indices = m_world.m_diagnostic_atlas ? m_world.m_diagnostic_atlas.get()
                                                                 : m_world.m_cell_dummy.get();
        rhi::IBuffer *const colors = m_world.m_diagnostic_colors ? m_world.m_diagnostic_colors.get()
                                                                 : m_world.m_terrain_palette.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_diagnostic_indices"].setBinding(rhi::Binding(indices, makeFullRange(indices))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_diagnostic_colors"].setBinding(rhi::Binding(colors, makeFullRange(colors))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_terrain_visuals"].setBinding(
                          rhi::Binding(m_world.m_terrain_palette, makeFullRange(m_world.m_terrain_palette))));
        rhi::IBuffer *const descriptors = m_world.m_texture_cache->descriptorBuffer();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_textures"].setBinding(rhi::Binding(descriptors, makeFullRange(descriptors))));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_sampler"].setDescriptorHandle(m_terrain_sampler_handle));
        if (not retained_table or not m_chunk_slot_uploaded[m_tile_ring_cursor]) {
            std::array<u32, kGridTileCacheCapacity> slots;
            slots.fill(0xffffffffu);
            if (retained_table) {
                slots = m_world.m_resident_chunk_slots;
            } else {
                for (u32 chunk = 0u; chunk < kGridTileCacheCapacity; ++chunk) {
                    if (m_world.m_cell_slots[chunk]) {
                        slots[chunk] = *m_world.m_cell_slots[chunk];
                    }
                }
            }
            std::memcpy(m_chunk_slot_mapped[m_tile_ring_cursor], slots.data(), sizeof(slots));
            m_chunk_slot_uploaded[m_tile_ring_cursor] = retained_table;
        }
        rhi::IBuffer *const map = m_chunk_slot_buffers[m_tile_ring_cursor].get();
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_chunk_slots"].setBinding(rhi::Binding(map, makeFullRange(map))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            cursor["g_contour_points"].setBinding(rhi::Binding(m_contour_points, makeFullRange(m_contour_points))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            cursor["g_contour_cases"].setBinding(rhi::Binding(m_contour_cases, makeFullRange(m_contour_cases))));
        return {};
    }

    std::error_code GridPass::rebuildResidentTable_() {
        Array<GridTilePayload> payloads;
        payloads.reserve(m_world.m_submitted_tiles.size());
        bool has_cells = false;
        for (const GridTileSubmission &tile: m_world.m_submitted_tiles) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSubmission_(tile));
            const auto cells = m_world.m_cell_cache.find(tile.m_chunk_id);
            if (cells != m_world.m_cell_cache.end()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, not cells->second.m_pending and m_world.m_cell_atlas != nullptr
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::operation_in_progress));
                has_cells = true;
            }
            GridTilePayload payload{};
            const auto cached = resolveCached(tile.m_chunk_id);
            if (cached) {
                payload = *cached;
            } else {
                payload.m_material = tile.m_material_id;
            }
            // Submission bounds are authoritative for both the reference cull
            // and GPU geometry, including clean submissions with cached data.
            applySubmissionBounds_(payload, tile.m_tile_range);
            applyCellPayload_(payload, tile.m_chunk_id);
            payloads.push_back(payload);
        }
        m_world.m_resident_cell_slots.reset();
        m_world.m_resident_chunk_slots.fill(0xffffffffu);
        for (u32 chunk = 0u; chunk < kGridTileCacheCapacity; ++chunk) {
            if (m_world.m_cell_slots[chunk]) {
                m_world.m_resident_chunk_slots[chunk] = *m_world.m_cell_slots[chunk];
                // Fragments can read diagonal neighbors absent from the compact
                // draw table. Pin the entire published lookup, not just draws.
                m_world.m_resident_cell_slots.set(*m_world.m_cell_slots[chunk]);
            }
        }
        m_chunk_slot_uploaded.fill(false);
        m_world.m_resident_table = std::move(payloads);
        m_world.m_resident_cell_size_m = m_world.m_cell_size_m;
        m_world.m_resident_contours_enabled = m_world.m_contours_enabled;
        m_world.m_resident_boundary_material_id = m_world.m_boundary_material_id;
        m_world.m_visibility_has_cells = has_cells;
        m_world.m_resident_table_stale = false;
        m_resident_table_uploaded.fill(false);
        ++m_resident_table_build_count;
        return {};
    }

    std::error_code GridPass::prepareGpuVisibility(const PreRenderContext &context) {
        m_visibility_prepared = false;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_world.m_submitted_tiles.size() <= kGridTileCacheCapacity
                          ? std::error_code{}
                          : std::make_error_code(std::errc::no_buffer_space));
        reclaimCompleted_(context.m_completed_fence_value);
        if (m_world.m_resident_table_stale and not m_world.m_cell_publication_deferred) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, rebuildResidentTable_());
        }
        const auto &payloads = m_world.m_resident_table;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(context.m_device, false));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureVisibilityBuffers_(context.m_device));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureTilePayloads_(
                          context.m_device, std::max<u32>(1u, safe_narrowing(payloads.size())),
                          context.m_completed_fence_value, context.m_wait_for_fence));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, selectSlot_(context.m_completed_fence_value, context.m_wait_for_fence));
        if (not payloads.empty() and not m_resident_table_uploaded[m_tile_ring_cursor]) {
            std::memcpy(
                m_tile_payload_mapped[m_tile_ring_cursor], payloads.data(), payloads.size() * sizeof(GridTilePayload));
            m_resident_table_uploaded[m_tile_ring_cursor] = true;
            ++m_resident_table_upload_count;
        }

        struct alignas(16) VisibilityFrame {
            float m_planes[6][4]{};
            u32 m_count{};
            float m_cell_size_m{1.0f};
            std::array<u32, 2u> m_padding{};
        };
        static_assert(sizeof(VisibilityFrame) == 112u);
        VisibilityFrame frame{};
        for (u32 plane = 0u; plane < 6u; ++plane) {
            std::memcpy(
                frame.m_planes[plane], m_camera_view.m_frustum.clip[plane].data(), sizeof(frame.m_planes[plane]));
        }
        frame.m_count = safe_narrowing(payloads.size());
        frame.m_cell_size_m = m_world.m_resident_cell_size_m;
        VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::UnorderedAccess);
        for (u32 phase = 0u; phase < 2u; ++phase) {
            VisibilityProgram &program = m_visibility_programs[phase];
            rhi::IShaderObject *const root = program.m_roots[m_tile_ring_cursor].get();
            rhi::ShaderCursor cursor{root};
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_args"].setBinding(rhi::Binding(slot.m_args, makeFullRange(slot.m_args))));
            if (phase == 1u) {
                rhi::ShaderCursor constants{};
                PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_visibility"].getDereferenced(constants));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor["g_visible_ids"].setBinding(rhi::Binding(slot.m_ids, makeFullRange(slot.m_ids))));
                rhi::IBuffer *const tiles = m_tile_payloads[m_tile_ring_cursor].get();
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor["g_tiles"].setBinding(rhi::Binding(tiles, makeFullRange(tiles))));
            }
            rhi::IComputePassEncoder *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(program.m_pipeline, root);
                pass->dispatchCompute(phase == 0u ? 1u : std::max(1u, (frame.m_count + 63u) / 64u), 1u, 1u);
                ++m_dispatch_commands;
            }
            context.m_encoder.globalBarrier();
        }
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::IndirectArgument);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::ShaderResource);
        m_visibility_prepared = true;
        return {};
    }

    std::error_code GridPass::renderGpuIndirect(const DrawContext &context) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            m_visibility_prepared ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));
        m_visibility_prepared = false;
        const auto cached = pipelineFor_(context.m_device, context.m_render_pipeline_key);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cached ? std::error_code{} : cached.error());
        rhi::IShaderObject *const root = (*cached)->m_slot_roots[m_tile_ring_cursor].get();
        rhi::ShaderCursor cursor{root};
        GridFrame frame{};
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_use_visible_ids = 1u;
        frame.m_surface_mode = m_world.m_resident_contours_enabled ? 1u : 0u;
        frame.m_surface_mode |= m_world.m_terrain_visuals.empty() ? 0u : 2u;
        frame.m_surface_mode |= (m_world.m_diagnostic_mode == EGridDiagnosticMode::initial_matter and
                                    not m_world.m_initial_matter_valid)
                                    ? 0u
                                    : static_cast<u32>(m_world.m_diagnostic_mode) << 2u;
        frame.m_boundary_material_id = m_world.m_resident_boundary_material_id;
        frame.m_cell_size_m = m_world.m_resident_cell_size_m;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, (*cached)->m_slot_frame_cursors[m_tile_ring_cursor].setData(&frame, sizeof(frame)));
        rhi::IBuffer *const tiles = m_tile_payloads[m_tile_ring_cursor].get();
        VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
        rhi::IBuffer *const cells = m_world.m_visibility_has_cells ? m_world.m_cell_atlas.get()
                                                                   : m_world.m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_tiles"].setBinding(rhi::Binding(tiles, makeFullRange(tiles))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_visible_ids"].setBinding(rhi::Binding(slot.m_ids, makeFullRange(slot.m_ids))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_cell_materials"].setBinding(rhi::Binding(cells, makeFullRange(cells))));
        m_cached_pipeline->m_slot_atlases[m_tile_ring_cursor] = cells == m_world.m_cell_atlas.get() ? cells : nullptr;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindSurfaceBuffers_(context.m_device, cursor, true));
        context.m_pass.bindPipeline((*cached)->m_pipeline, root);
        context.m_pass.drawIndirect(1u, rhi::BufferOffsetPair{slot.m_args});
        ++m_draw_commands;
        return {};
    }

    std::error_code GridPass::uploadFrameConstants_(rhi::ShaderCursor &frame_cursor) {
        GridFrame frame{};
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_cell_size_m = m_world.m_cell_size_m;
        frame.m_surface_mode = m_world.m_contours_enabled ? 1u : 0u;
        frame.m_surface_mode |= m_world.m_terrain_visuals.empty() ? 0u : 2u;
        frame.m_surface_mode |= (m_world.m_diagnostic_mode == EGridDiagnosticMode::initial_matter and
                                    not m_world.m_initial_matter_valid)
                                    ? 0u
                                    : static_cast<u32>(m_world.m_diagnostic_mode) << 2u;
        frame.m_boundary_material_id = m_world.m_boundary_material_id;

        PPR_RETURN_ERROR_ON_FAIL(GridPass, frame_cursor.setData(&frame, sizeof(GridFrame)));

        return default_value_v;
    }
}
