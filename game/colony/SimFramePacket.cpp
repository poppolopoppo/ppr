module;

#include "Colony.Elements.h"
#include "pP/Macros.h"

module game.colony.sim_packet;

import engine.core;
import engine.sim;
import engine.asset;
import game.colony.content;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(SimPresentation, info, none)

    // ------------------------------------------------------------------
    // presentation capture
    // ------------------------------------------------------------------

    namespace {
        std::error_code captureCatalogPresentation_(
            SimFramePacket &packet, const ContentPresentation &presentation, const std::string_view profile_key,
            const u32 boundary_material, const u32 cells_per_metre, const double reference_thickness) {
            const hal::ProfileScope scope{"Game.SimPacket.Catalog"};

            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation,
                not presentation.catalog() or not presentation.catalog()->contentFingerprint() or cells_per_metre == 0u
                    ? std::make_error_code(std::errc::invalid_argument)
                    : std::error_code{});

            const auto &content = *presentation.catalog();
            packet.m_cell_size_m = 1.0f / static_cast<float>(cells_per_metre);
            packet.m_has_catalog = true;
            packet.m_boundary_material_id = boundary_material;
            packet.m_content_fingerprint = presentation.fingerprint();

            for (const auto &texture: presentation.textures()) {
                packet.m_terrain_images.push_back(texture.m_image);
            }

            for (const MaterialDefinition &material: content.materials()) {
                if (material.m_numeric_id == 0u) {
                    continue;
                }
                if (material.m_representation == EMaterialRepresentation::particle) {
                    packet.m_particle_material_ids.push_back(material.m_numeric_id);
                }
                GridTerrainVisual visual{};
                visual.m_material_id = material.m_numeric_id;
                visual.m_color_rgba = material.m_color_rgba;
                visual.m_roundness = static_cast<float>(material.m_roundness);
                visual.m_texture_period_m = static_cast<float>(material.m_texture_period_m.value_or(1.0));
                visual.m_metallic = static_cast<float>(material.m_metallic);
                visual.m_roughness = static_cast<float>(material.m_roughness);
                visual.m_opacity = 1.0f;
                for (u32 c = 0u; c < 3u; ++c) {
                    visual.m_emissive_rgb[c] = static_cast<float>(material.m_emissive_rgb[c]);
                }
                visual.m_emissive_strength = static_cast<float>(material.m_emissive_strength);
                visual.m_ior = static_cast<float>(material.m_ior);
                visual.m_normal_scale = static_cast<float>(material.m_normal_scale);
                visual.m_refraction_depth_m = static_cast<float>(material.m_refraction_depth_m);
                visual.m_surface_flags =
                    (material.m_alpha_mode == EMaterialAlphaMode::blend ? kGridSurfaceOpticalBlend : 0u) |
                    (material.m_texture_sampling == EMaterialTextureSampling::repeat ? kGridSurfaceTextureRepeat : 0u);
                if (material.m_albedo_texture) {
                    const auto textures = presentation.textures();
                    for (u32 index = 0u; index < textures.size(); ++index) {
                        if (textures[index].m_path == *material.m_albedo_texture and not textures[index].m_normal_map) {
                            visual.m_texture_index = index;
                            break;
                        }
                    }

                    PPR_RETURN_ERROR_ON_FAIL(
                        SimPresentation, visual.m_texture_index == 0xffffffffu
                                             ? std::make_error_code(std::errc::invalid_argument)
                                             : std::error_code{});
                }

                if (material.m_normal_texture) {
                    const auto textures = presentation.textures();
                    for (u32 index = 0u; index < textures.size(); ++index) {
                        if (textures[index].m_path == *material.m_normal_texture and textures[index].m_normal_map) {
                            visual.m_normal_texture_index = index;
                            break;
                        }
                    }

                    PPR_RETURN_ERROR_ON_FAIL(
                        SimPresentation, visual.m_normal_texture_index == 0xffffffffu
                                             ? std::make_error_code(std::errc::invalid_argument)
                                             : std::error_code{});
                }

                packet.m_terrain_visuals.push_back(visual);
            }

            packet.m_reference_thickness_m = static_cast<float>(reference_thickness);
            const auto *const profile = content.findWorld(profile_key);
            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation, not profile ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});
            const auto stack = std::ranges::find(
                content.backgrounds(), profile->m_background_stack, &BackgroundStackDefinition::m_key);
            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation, stack == content.backgrounds().end()
                                     ? std::make_error_code(std::errc::invalid_argument)
                                     : std::error_code{});

            packet.m_has_background = true;
            for (const auto &layer: stack->m_layers) {
                auto &target = packet.m_background_layers[layer.m_order];
                target.m_order = layer.m_order;
                target.m_color_rgba = layer.m_color_rgba;
                target.m_repeat_period_m = static_cast<float>(layer.m_repeat_period_m);
                target.m_parallax_factor = static_cast<float>(layer.m_parallax_factor);
                const auto textures = presentation.textures();
                for (u32 index = 0u; index < textures.size(); ++index) {
                    if (textures[index].m_path == layer.m_texture and not textures[index].m_normal_map) {
                        target.m_texture_index = index;
                        break;
                    }
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    SimPresentation, target.m_texture_index == 0xffffffffu
                                         ? std::make_error_code(std::errc::invalid_argument)
                                         : std::error_code{});
            }

            return {};
        }

        std::error_code capturePresentation_(
            SimFramePacket &packet, const ColonyDriver &driver,
            ParticlePresentationCache *const particle_cache = nullptr) {
            const hal::ProfileScope scope{"Game.SimPacket.Presentation"};

            packet.m_initial_matter_valid = driver.initialMatterBaselineValid();
            packet.m_cell_size_m = 1.0f / static_cast<float>(driver.cellsPerMetre());
            const ContentCatalog *const catalog = driver.contentCatalog();
            if (catalog == nullptr) {
                return {};
            }

            const auto *world = driver.preparedWorld();
            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation,
                not world or
                        not world->presentation() or
                        world->presentation()->catalog().get() != catalog or
                        world->presentation()->generation() != asset::RequestGeneration{driver.worldGeneration()}
                    ? std::make_error_code(std::errc::invalid_argument)
                    : std::error_code{});

            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation, captureCatalogPresentation_(
                                     packet, *world->presentation(), driver.profileKey(), driver.boundaryMaterial(),
                                     driver.cellsPerMetre(), driver.referenceThicknessMetres()));

            ParticlePresentationCache temporary;
            auto &cache = particle_cache ? *particle_cache : temporary;
            packet.m_particle_batches = cache.capture(driver);
            packet.m_surface_profile = cache.captureSurface(driver);
            return {};
        }

        [[nodiscard]] std::error_code validatePresentation_(const SimFramePacket &packet) {
            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation, (packet.m_cell_size_m == 1.0f or packet.m_cell_size_m == 1.0f / 3.0f)
                                     ? std::error_code{}
                                     : std::make_error_code(std::errc::invalid_argument));
            if (not packet.m_has_catalog) {
                PPR_RETURN_ERROR_ON_FAIL(
                    SimPresentation, packet.m_terrain_visuals.empty() and
                                             packet.m_terrain_images.empty() and
                                             packet.m_content_fingerprint == 0u
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::invalid_argument));
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation, packet.m_terrain_visuals.size() <= kMaxContentMaterials
                                     ? std::error_code{}
                                     : std::make_error_code(std::errc::invalid_argument));

            for (std::size_t index = 0u; index < packet.m_terrain_visuals.size(); ++index) {
                const auto &visual = packet.m_terrain_visuals[index];
                PPR_RETURN_ERROR_ON_FAIL(
                    SimPresentation, visual.m_material_id > 0u and
                                             visual.m_material_id < 0xffffu and
                                             std::isfinite(visual.m_texture_period_m) and
                                             visual.m_texture_period_m >= 4096.0f / (std::numeric_limits<float>::max)()
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::invalid_argument));
                PPR_RETURN_ERROR_ON_FAIL(
                    SimPresentation, (visual.m_texture_index == 0xffffffffu or
                                         visual.m_texture_index < packet.m_terrain_images.size()) and
                                             (visual.m_normal_texture_index == 0xffffffffu or
                                                 visual.m_normal_texture_index < packet.m_terrain_images.size())
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::invalid_argument));
                for (std::size_t previous = 0u; previous < index; ++previous) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        SimPresentation, packet.m_terrain_visuals[previous].m_material_id != visual.m_material_id
                                             ? std::error_code{}
                                             : std::make_error_code(std::errc::invalid_argument));
                }
            }

            const bool boundary_known = std::ranges::any_of(
                packet.m_terrain_visuals, [&packet](const GridTerrainVisual &visual) {
                    return visual.m_material_id == packet.m_boundary_material_id;
                });
            PPR_RETURN_ERROR_ON_FAIL(
                SimPresentation,
                boundary_known ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            return {};
        }
        [[nodiscard]] GridTileRange tileRange(const sim::ChunkPos pos) noexcept {
            const i32 min_x = static_cast<i32>(pos.m_x * sim::kChunkEdge);
            const i32 min_y = static_cast<i32>(pos.m_y * sim::kChunkEdge);
            return {min_x, min_y, min_x + static_cast<i32>(sim::kChunkEdge), min_y + static_cast<i32>(sim::kChunkEdge)};
        }

        [[nodiscard]] bool containsChunk(const Array<sim::ChunkPos> &chunks, const sim::ChunkPos pos) noexcept {
            for (const sim::ChunkPos chunk: chunks) {
                if (chunk == pos) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const Array<sim::Cell> *findCells(
            const Array<SimChunkCells> &payloads, const sim::ChunkPos pos) noexcept {
            for (const SimChunkCells &payload: payloads) {
                if (payload.m_pos == pos) {
                    return &payload.m_cells;
                }
            }
            return nullptr;
        }

        // Prepared worlds retain catalog IDs, including vacuum zero. Explicit
        // legacy fixtures retain the translator's transparent sentinel.
        [[nodiscard]] std::error_code convertCells(
            const std::span<const sim::Cell> cells, const SimFramePacket &packet, Array<u16> &scratch) {
            std::bitset<65536u> terrain_ids{};

            if (packet.m_has_catalog) {
                for (const GridTerrainVisual &visual: packet.m_terrain_visuals) {
                    if (std::ranges::find(packet.m_particle_material_ids, visual.m_material_id) ==
                        packet.m_particle_material_ids.end()) {
                        terrain_ids.set(visual.m_material_id);
                    }
                }
            }

            scratch.resize(sim::kCellsPerChunk);

            for (std::size_t cell = 0u; cell < sim::kCellsPerChunk; ++cell) {
                const u16 element = cells[cell].m_element;
                if (element != kElementVacuum) {
                    const bool known = packet.m_has_catalog ? terrain_ids.test(element) : element <= kElementVent;
                    PPR_RETURN_ERROR_ON_FAIL(
                        SimPresentation, known ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
                }
                scratch[cell] = not packet.m_has_catalog and element == kElementVacuum ? u16{0xffffu} : element;
            }
            return {};
        }
    }

    // ------------------------------------------------------------------
    // packet assembly
    // ------------------------------------------------------------------

    Expected<SimFramePacket> captureCatalogPresentation(
        const ContentPresentation &presentation, const std::string_view profile, const u32 boundary_material,
        const u32 cells_per_metre, const double reference_thickness) {
        SimFramePacket packet{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            SimPresentation,
            captureCatalogPresentation_(
                packet, presentation, profile, boundary_material, cells_per_metre, reference_thickness));
        return packet;
    }
    Array<GridParticleBatch> ParticlePresentationCache::capture(const ColonyDriver &driver) {
        if (m_generation != driver.worldGeneration()) {
            m_chunks = {};
            m_surface_profile.reset();
            m_surface_captured = false;
            m_generation = driver.worldGeneration();
        }

        std::array<std::shared_ptr<const Array<sim::MatterParticle>>, sim::kChunkCount> sources{};
        const auto snapshot = driver.captureParticles();

        for (const auto &batch: snapshot.m_chunks) {
            sources[sim::chunkIndexOf(batch.m_position)] = batch.m_particles;
        }

        for (const auto &batch: driver.editParticles()) {
            sources[sim::chunkIndexOf(batch.m_position)] = batch.m_particles;
        }

        Array<GridParticleBatch> result;

        for (u32 index = 0u; index < sim::kChunkCount; ++index) {
            auto &entry = m_chunks[index];
            if (entry.m_source != sources[index]) {
                Array<GridParticleInstance> particles;
                if (sources[index]) {
                    particles.reserve(sources[index]->size());
                    for (const auto &particle: *sources[index]) {
                        particles.push_back(
                            {particle.m_id, particle.m_x_m, particle.m_y_m, particle.m_volume_m3, particle.m_mass_kg,
                                particle.m_temperature_k, particle.m_material, static_cast<u32>(particle.m_phase)});
                    }
                }
                entry.m_target = std::make_shared<const Array<GridParticleInstance>>(std::move(particles));
                entry.m_source = sources[index];
            }
            if (entry.m_target) {
                result.push_back({index, entry.m_target});
            }
        }
        return result;
    }

    std::shared_ptr<const GridSurfaceProfile> ParticlePresentationCache::captureSurface(const ColonyDriver &driver) {
        if (m_generation != driver.worldGeneration()) {
            m_chunks = {};
            m_surface_profile.reset();
            m_surface_captured = false;
            m_generation = driver.worldGeneration();
        }

        if (m_surface_captured) {
            return m_surface_profile;
        }

        static_assert(kGridSurfaceColumns == sim::kWorldEdge);
        std::array<float, kGridSurfaceColumns> heights;

        if (not driver.sampleSurfaceHeights(heights)) {
            m_surface_captured = true;
            return {};
        }

        auto surface = std::make_shared<GridSurfaceProfile>();
        surface->m_cell_size_m = 1.0f / static_cast<float>(driver.cellsPerMetre());

        for (u32 column = 0u; column < kGridSurfaceColumns; ++column) {
            surface->m_heights_m[column / 4u][column % 4u] = heights[column];
        }

        m_surface_profile = std::move(surface);
        m_surface_captured = true;
        return m_surface_profile;
    }

    Expected<SimFramePacket> makeSimFramePacket(
        const u64 frame_id, const ColonyDriver &driver, const bool full_refresh,
        ParticlePresentationCache *const particles) {
        SimFramePacket packet{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, capturePresentation_(packet, driver, particles));

        packet.m_frame_id = frame_id;
        packet.m_seed = driver.seed();
        packet.m_world_generation = driver.worldGeneration();
        packet.m_tick = driver.tickCount();
        packet.m_dig_total = driver.digTotal();
        packet.m_agent_count = static_cast<u32>(driver.agentSummaries().size());
        packet.m_path_counts = driver.pathCounts();
        packet.m_errands = driver.errandCount();
        packet.m_full_refresh = full_refresh;

        for (const ColonyDriver::DirtyChunk &row: driver.presentEditsCapture()) {
            packet.m_present_chunks.push_back(row.m_pos);
            packet.m_present_revisions.push_back(row.m_revision);
        }

        const sim::ChunkGrid &grid = driver.grid();
        for (u32 y = 0u; y < sim::kChunksPerEdge; ++y) {
            for (u32 x = 0u; x < sim::kChunksPerEdge; ++x) {
                const sim::ChunkPos pos{x, y};
                if (not grid.isResident(pos)) {
                    continue;
                }
                packet.m_resident_chunks.push_back(pos);
                if (not full_refresh and not containsChunk(packet.m_present_chunks, pos)) {
                    continue;
                }
                const std::optional<std::span<const sim::Cell>> cells = grid.residentCells(pos);
                if (not cells or cells->size() != sim::kCellsPerChunk) [[unlikely]] {
                    continue;
                }
                SimChunkCells payload{};
                payload.m_pos = pos;
                payload.m_cells.assign(cells->begin(), cells->end());
                packet.m_chunk_cells.push_back(std::move(payload));
            }
        }

        if (driver.editReadyForPresentation()) {
            packet.m_initial_matter_valid = false;
            for (const auto &row: driver.editManifest()) {
                const auto found = std::ranges::find(packet.m_present_chunks, row.m_pos);
                if (found == packet.m_present_chunks.end()) {
                    packet.m_present_chunks.push_back(row.m_pos);
                    packet.m_present_revisions.push_back(row.m_revision);
                } else {
                    packet.m_present_revisions[static_cast<std::size_t>(found - packet.m_present_chunks.begin())] =
                        row.m_revision;
                }
            }
            for (const auto &chunk: driver.editChunks()) {
                const auto cells = chunk.residentCells();
                PPR_ASSERT(cells.has_value());
                if (not containsChunk(packet.m_resident_chunks, chunk.position())) {
                    packet.m_resident_chunks.push_back(chunk.position());
                }
                std::erase_if(packet.m_chunk_cells, [&](const SimChunkCells &payload) {
                    return payload.m_pos == chunk.position();
                });
                SimChunkCells payload{};
                payload.m_pos = chunk.position();
                payload.m_cells.assign(cells->begin(), cells->end());
                packet.m_chunk_cells.push_back(std::move(payload));
            }
        }
        return packet;
    }

    bool isSimPacketCoherent(
        const SimFramePacket &packet, const u64 live_tick, const u64 live_world_generation) noexcept {
        return packet.m_tick == live_tick and packet.m_world_generation == live_world_generation;
    }

    Expected<SimFramePacket> makeInitialPresentationBatch(
        const u64 frame_id, const ColonyDriver &driver, const u32 first_chunk, const u32 chunk_count,
        ParticlePresentationCache *const particles) {
        if (chunk_count == 0u or
            chunk_count > kInitialPresentationBatchChunks or
            first_chunk >= sim::kChunkCount or
            chunk_count > sim::kChunkCount - first_chunk) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, std::errc::invalid_argument);
        }
        SimFramePacket packet{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, capturePresentation_(packet, driver, particles));

        packet.m_frame_id = frame_id;
        packet.m_seed = driver.seed();
        packet.m_world_generation = driver.worldGeneration();
        packet.m_tick = driver.tickCount();
        packet.m_agent_count = static_cast<u32>(driver.agentSummaries().size());

        const auto dirty = driver.presentEditsCapture();
        const auto &grid = driver.grid();
        const u32 end = first_chunk + chunk_count;

        for (u32 index = 0u; index < end; ++index) {
            const sim::ChunkPos pos{index % sim::kChunksPerEdge, index / sim::kChunksPerEdge};
            if (not grid.isResident(pos)) {
                continue;
            }
            packet.m_resident_chunks.push_back(pos);
            if (index < first_chunk) {
                continue;
            }
            const auto cells = grid.residentCells(pos);
            if (not cells or cells->size() != sim::kCellsPerChunk) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, std::errc::state_not_recoverable);
            }
            u64 revision{};
            for (const auto &row: dirty) {
                if (row.m_pos == pos) {
                    revision = row.m_revision;
                    break;
                }
            }
            packet.m_present_chunks.push_back(pos);
            packet.m_present_revisions.push_back(revision);
            SimChunkCells payload{.m_pos = pos};
            payload.m_cells.assign(cells->begin(), cells->end());
            packet.m_chunk_cells.push_back(std::move(payload));
        }
        return packet;
    }

    Expected<GridPass::GridStagedFrame> captureWorldPresentation(
        const ColonyDriver &driver, const IContext *const context) {
        SimFramePacket presentation{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, capturePresentation_(presentation, driver));
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, validatePresentation_(presentation));

        const auto dirty = driver.presentEditsCapture();
        const auto &grid = driver.grid();

        GridPass::GridStagedFrame stage{};
        stage.m_ack_seed = driver.worldGeneration();
        stage.m_full_refresh = true;

        for (u32 index = 0u; index < sim::kChunkCount; ++index) {
            if (context != nullptr) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, context->error());
            }
            const sim::ChunkPos pos{index % sim::kChunksPerEdge, index / sim::kChunksPerEdge};
            if (not grid.isResident(pos)) {
                continue;
            }
            const auto cells = grid.residentCells(pos);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SimPresentation, cells and cells->size() == sim::kCellsPerChunk
                                     ? std::error_code{}
                                     : std::make_error_code(std::errc::state_not_recoverable));
            const auto range = tileRange(pos);
            stage.m_tiles.push_back(GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = range, .m_chunk_id = index});
            GridPass::GridStagedUpload upload{};
            upload.m_chunk_id = index;
            upload.m_payload.m_rect[0] = static_cast<float>(range.m_min_x);
            upload.m_payload.m_rect[1] = static_cast<float>(range.m_min_y);
            upload.m_payload.m_rect[2] = static_cast<float>(range.m_max_x);
            upload.m_payload.m_rect[3] = static_cast<float>(range.m_max_y);
            PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, convertCells(*cells, presentation, upload.m_cell_materials));
            stage.m_uploads.push_back(std::move(upload));
            for (const auto &row: dirty) {
                if (row.m_pos == pos) {
                    stage.m_ack_chunk_ids.push_back(index);
                    stage.m_ack_revisions.push_back(row.m_revision);
                    break;
                }
            }
        }

        if (context != nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, context->error());
        }

        stage.m_cell_size_m = presentation.m_cell_size_m;
        stage.m_has_catalog = presentation.m_has_catalog;
        stage.m_initial_matter_valid = presentation.m_initial_matter_valid;
        stage.m_boundary_material_id = presentation.m_boundary_material_id;
        stage.m_content_fingerprint = presentation.m_content_fingerprint;
        stage.m_terrain_visuals = std::move(presentation.m_terrain_visuals);
        stage.m_terrain_images = std::move(presentation.m_terrain_images);
        stage.m_reference_thickness_m = presentation.m_reference_thickness_m;
        stage.m_has_background = presentation.m_has_background;
        stage.m_background_layers = presentation.m_background_layers;
        stage.m_surface_profile = presentation.m_surface_profile;
        stage.m_particle_batches = presentation.m_particle_batches;
        return stage;
    }

    // ------------------------------------------------------------------
    // initial presentation
    // ------------------------------------------------------------------

    u32 InitialPresentation::completedChunks() const noexcept {
        return m_next_chunk;
    }

    Expected<GridPass::GridStagedFrame> InitialPresentation::stage(const u64 frame_id, const ColonyDriver &driver) {
        GridPass::GridStagedFrame stage{};
        SimFramePacket presentation{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, capturePresentation_(presentation, driver, &m_particle_cache));

        stage.m_cell_size_m = presentation.m_cell_size_m;
        stage.m_has_catalog = presentation.m_has_catalog;
        stage.m_initial_matter_valid = presentation.m_initial_matter_valid;
        stage.m_boundary_material_id = presentation.m_boundary_material_id;
        stage.m_content_fingerprint = presentation.m_content_fingerprint;
        stage.m_terrain_visuals = std::move(presentation.m_terrain_visuals);
        stage.m_terrain_images = std::move(presentation.m_terrain_images);
        stage.m_reference_thickness_m = presentation.m_reference_thickness_m;
        stage.m_has_background = presentation.m_has_background;
        stage.m_background_layers = presentation.m_background_layers;
        stage.m_surface_profile = presentation.m_surface_profile;
        stage.m_particle_batches = presentation.m_particle_batches;

        if (m_frame or m_next_chunk == sim::kChunkCount) {
            stage.m_tiles = m_visible;
            return stage;
        }

        const u32 count = std::min(kInitialPresentationBatchChunks, sim::kChunkCount - m_next_chunk);
        auto packet = makeInitialPresentationBatch(frame_id, driver, m_next_chunk, count, &m_particle_cache);
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, packet);
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, buildGridStagedFrame(*packet, stage));

        stage.m_reset_caches = m_next_chunk == 0u and not m_reset_applied;
        m_pending = stage.m_tiles;
        for (auto &tile: m_pending) {
            tile.m_dirty_mask = 0u;
        }
        m_frame = frame_id;
        return stage;
    }

    bool InitialPresentation::consume(const RenderFrameResult &result, const u64 world_generation) {
        if (m_frame != result.m_frame_id or result.m_grid_ack_seed != world_generation) {
            return false;
        }
        m_frame.reset();
        if (result.m_grid_staging_applied and result.m_grid_reset_caches) {
            m_reset_applied = true;
        }
        if (not result.m_presented or not result.m_grid_stage_applied or result.m_error or result.m_dropped) {
            return false;
        }
        m_visible = std::move(m_pending);
        m_next_chunk = std::min(m_next_chunk + kInitialPresentationBatchChunks, sim::kChunkCount);
        return m_next_chunk == sim::kChunkCount;
    }

    // ------------------------------------------------------------------
    // grid staging
    // ------------------------------------------------------------------

    std::error_code buildGridStagedFrame(const SimFramePacket &packet, GridPass::GridStagedFrame &stage) {
        if (const std::error_code error = validatePresentation_(packet)) {
            stage = {};
            PPR_RETURN_ERROR_ON_FAIL(SimPresentation, error);
        }

        stage.m_cell_size_m = packet.m_cell_size_m;
        stage.m_has_catalog = packet.m_has_catalog;
        stage.m_initial_matter_valid = packet.m_initial_matter_valid;
        stage.m_boundary_material_id = packet.m_boundary_material_id;
        stage.m_content_fingerprint = packet.m_content_fingerprint;
        stage.m_terrain_visuals = packet.m_terrain_visuals;
        stage.m_terrain_images = packet.m_terrain_images;
        stage.m_reference_thickness_m = packet.m_reference_thickness_m;
        stage.m_has_background = packet.m_has_background;
        stage.m_background_layers = packet.m_background_layers;
        stage.m_surface_profile = packet.m_surface_profile;
        stage.m_particle_batches = packet.m_particle_batches;

        // Owned payload only: no driver ack, no translator contact, and no
        // pass mutation happen on the client. The pass-side contact runs
        // on the pass's owner lane via GridPass::applyStagedFrame.
        // The consumption-ack manifest rides inside the owned stage: the
        // lane only round-trips it into the RenderFrameResult, and the
        // derived owner consumes it at settlement. Derived from the owned
        // packet only — never the live driver state.
        stage.m_full_refresh = packet.m_full_refresh;
        stage.m_ack_seed = packet.m_world_generation;
        if (packet.m_present_chunks.size() != packet.m_present_revisions.size()) [[unlikely]] {
            stage.m_ack_chunk_ids.clear();
            stage.m_ack_revisions.clear();
            return std::make_error_code(std::errc::invalid_argument);
        }
        stage.m_ack_chunk_ids.clear();
        stage.m_ack_chunk_ids.reserve(packet.m_present_chunks.size());
        stage.m_ack_revisions.clear();
        stage.m_ack_revisions.reserve(packet.m_present_revisions.size());
        for (u64 i = 0u; i < packet.m_present_chunks.size(); ++i) {
            stage.m_ack_chunk_ids.push_back(sim::chunkIndexOf(packet.m_present_chunks[i]));
            stage.m_ack_revisions.push_back(packet.m_present_revisions[i]);
        }

        // Stage exactly the capture-time residency. Dirty marks come from
        // the packet (or the full-refresh flag), never from translator or
        // live state, so post-capture mutation cannot alter the frame.
        stage.m_tiles.clear();
        stage.m_tiles.reserve(packet.m_resident_chunks.size());
        for (const sim::ChunkPos pos: packet.m_resident_chunks) {
            const bool dirty = packet.m_full_refresh or containsChunk(packet.m_present_chunks, pos);
            stage.m_tiles.push_back(
                GridTileSubmission{
                    .m_dirty_mask = dirty ? 1u : 0u,
                    .m_tile_range = tileRange(pos),
                    .m_chunk_id = sim::chunkIndexOf(pos),
                    .m_material_id = 0u,
                });
        }

        // Validate + convert the captured contents now so a corrupt payload
        // fails closed on the client (never as a half-applied pass). The
        // converted u16 words and the one-element tile payload travel as
        // owned copies, ready for applyStagedFrame's requestUpload call.
        stage.m_uploads.clear();
        stage.m_uploads.reserve(packet.m_present_chunks.size() + packet.m_chunk_cells.size());
        for (const GridTileSubmission &tile: stage.m_tiles) {
            if (tile.m_dirty_mask == 0u) {
                continue;
            }
            const sim::ChunkPos pos{tile.m_chunk_id % sim::kChunksPerEdge, tile.m_chunk_id / sim::kChunksPerEdge};
            const Array<sim::Cell> *const cells = findCells(packet.m_chunk_cells, pos);
            if (cells == nullptr or cells->size() != sim::kCellsPerChunk) [[unlikely]] {
                stage.m_tiles.clear();
                stage.m_uploads.clear();
                stage.m_full_refresh = false;
                stage.m_ack_seed = 0u;
                stage.m_ack_chunk_ids.clear();
                stage.m_ack_revisions.clear();
                return std::make_error_code(std::errc::invalid_argument);
            }
            Array<u16> converted{};
            if (const std::error_code error = convertCells(*cells, packet, converted)) [[unlikely]] {
                stage.m_tiles.clear();
                stage.m_uploads.clear();
                stage.m_full_refresh = false;
                stage.m_ack_seed = 0u;
                stage.m_ack_chunk_ids.clear();
                stage.m_ack_revisions.clear();
                return error;
            }
            GridPass::GridStagedUpload upload{};
            upload.m_chunk_id = tile.m_chunk_id;
            upload.m_payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            upload.m_payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            upload.m_payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            upload.m_payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            upload.m_payload.m_material = 0u;
            upload.m_cell_materials = std::move(converted);
            stage.m_uploads.push_back(std::move(upload));
        }
        return {};
    }
}
