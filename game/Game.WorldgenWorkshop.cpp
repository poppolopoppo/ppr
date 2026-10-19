module;
#include "pP/Macros.h"
module game.application;
import :worldgen_workshop;
import game.colony.sim_packet;
import game.colony.metric;
import game.colony.worldgen;
import engine.sim;
import engine.asset;
import std;

namespace pP::colony {

    // ------------------------------------------------------------------
    // preview presentation capture
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(Workshop, info, none)
    Expected<GridPass::GridStagedFrame> captureWorldgenPreviewPresentation(
        const WorldgenPreviewResult &preview, const EGridDiagnosticMode mode, const u64 revision,
        const IContext &context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, context.error());
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Workshop, mode <= EGridDiagnosticMode::temperature and revision != 0u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Workshop, not preview.m_catalog or
                              not preview.m_presentation or
                              preview.m_presentation->catalog() != preview.m_catalog or
                              preview.m_presentation_generation != asset::RequestGeneration{revision} or
                              preview.m_presentation->generation() != preview.m_presentation_generation
                          ? std::make_error_code(std::errc::invalid_argument)
                          : std::error_code{});

        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, validateRuntimeWorldParameters(preview.m_plan.recipe()));

        auto captured = captureCatalogPresentation(
            *preview.m_presentation, preview.m_spec.m_profile, preview.m_plan.recipe().m_boundary_material,
            kFineCellsPerMetre, kReferenceThicknessMetres);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, captured);

        auto packet = std::move(*captured);
        packet.m_world_generation = *preview.m_presentation_generation;
        packet.m_full_refresh = true;

        // Preview particles already carry canonical production IDs. Convert the
        // bounded immutable batches without constructing a partial game driver.
        for (const auto &batch: preview.m_particles.m_chunks) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, context.error());
            Array<GridParticleInstance> particles;
            particles.reserve(batch.m_particles->size());
            for (const auto &particle: *batch.m_particles) {
                particles.push_back(
                    {particle.m_id, particle.m_x_m, particle.m_y_m, particle.m_volume_m3, particle.m_mass_kg,
                        particle.m_temperature_k, particle.m_material, static_cast<u32>(particle.m_phase)});
            }
            packet.m_particle_batches.push_back(
                {sim::chunkIndexOf(batch.m_position),
                    std::make_shared<const Array<GridParticleInstance>>(std::move(particles))});
        }

        if (preview.m_plan.recipe().m_open_surface) {
            auto surface = std::make_shared<GridSurfaceProfile>();
            surface->m_cell_size_m = packet.m_cell_size_m;
            for (u32 column = 0u; column < kGridSurfaceColumns; ++column) {
                surface->m_heights_m[column / 4u][column % 4u] = static_cast<float>(sim::generationSurfaceHeight(
                    preview.m_plan, (static_cast<double>(column) + 0.5) * static_cast<double>(packet.m_cell_size_m)));
            }
            packet.m_surface_profile = std::move(surface);
        }

        for (u32 cy = preview.m_support.m_bottom / sim::kChunkEdge; cy < preview.m_support.m_top / sim::kChunkEdge;
            ++cy) {
            for (u32 cx = preview.m_support.m_left / sim::kChunkEdge; cx < preview.m_support.m_right / sim::kChunkEdge;
                ++cx) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, context.error());
                const sim::ChunkPos position{cx, cy};
                const auto cells = preview.m_grid.residentCells(position);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Workshop,
                    cells.has_value() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
                packet.m_resident_chunks.push_back(position);
                packet.m_chunk_cells.push_back({position, Array<sim::Cell>{cells->begin(), cells->end()}});
            }
        }

        GridPass::GridStagedFrame stage{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, buildGridStagedFrame(packet, stage));

        // GridPass requires complete chunk ranges. The client marks the visible
        // interior separately from the retained contour/formation support context.
        stage.m_diagnostic_mode = mode;
        stage.m_diagnostic_revision = revision;

        if (mode == EGridDiagnosticMode::material) {
            stage.m_diagnostic_palette.resize(65536u);
            for (const auto &material: preview.m_catalog->materials()) {
                stage.m_diagnostic_palette[material.m_numeric_id].m_color_rgba = material.m_color_rgba;
            }
        } else if (mode == EGridDiagnosticMode::biome) {
            stage.m_diagnostic_palette.push_back({});
            for (const auto &biome: preview.m_catalog->biomes()) {
                stage.m_diagnostic_palette.push_back({biome.m_preview_color_rgba});
            }
        } else if (mode == EGridDiagnosticMode::temperature) {
            stage.m_diagnostic_palette.push_back({0x202020ffu});
            for (u32 bin = 0u; bin < 256u; ++bin) {
                const u32 green = 255u - static_cast<u32>(std::abs(2 * static_cast<i32>(bin) - 255));
                stage.m_diagnostic_palette.push_back({(bin << 24u) | (green << 16u) | ((255u - bin) << 8u) | 255u});
            }
        }

        if (mode == EGridDiagnosticMode::biome or mode == EGridDiagnosticMode::temperature) {
            for (auto &upload: stage.m_uploads) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, context.error());
                const sim::ChunkPos position{
                    upload.m_chunk_id % sim::kChunksPerEdge, upload.m_chunk_id / sim::kChunksPerEdge};
                const auto cells = preview.m_grid.residentCells(position);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Workshop,
                    cells.has_value() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

                upload.m_diagnostic_indices.resize(sim::kCellsPerChunk);
                for (u32 i = 0u; i < sim::kCellsPerChunk; ++i) {
                    if (mode == EGridDiagnosticMode::biome) {
                        const double x = (position.m_x * sim::kChunkEdge + i % sim::kChunkEdge + 0.5) * kFineCellMetres;
                        const double y = (position.m_y * sim::kChunkEdge + i / sim::kChunkEdge + 0.5) * kFineCellMetres;

                        upload.m_diagnostic_indices[i] = static_cast<u16>(
                            sim::generationBiomeAt(preview.m_plan, preview.m_layout, x, y) + 1u);
                    } else if ((*cells)[i].m_element != 0u) {
                        const double unit = std::clamp(
                            (static_cast<double>((*cells)[i].m_temperature) - 200.0) / 800.0, 0.0, 1.0);

                        upload.m_diagnostic_indices[i] = static_cast<u16>(
                            std::min(255u, static_cast<u32>(unit * 256.0)) + 1u);
                    }
                }
            }
        }
        return stage;
    }

    // ------------------------------------------------------------------
    // owned request values
    // ------------------------------------------------------------------

    struct WorldgenWorkshop::Request {
        enum class Kind : u8 {
            open,
            validate,
            preview,
            save,
            export_preview
        };
        u64 m_id{};
        u64 m_replaced_revision{};
        std::filesystem::path m_path{};
        Array<std::filesystem::path> m_roots{};
        std::optional<ContentAuthoringSnapshot> m_snapshot{};
        WorldgenPreviewSpec m_spec{};
        WorldgenPreviewDisplay m_display{};
        std::shared_ptr<State> m_export{};
        ContentSourceEpochToken m_source_epoch{};
        EGridDiagnosticMode m_mode{};
        Kind m_kind{};
    };

    struct WorldgenWorkshop::State {
        Request m_request;
        SharedContext m_context;
        context::CancelClauseFunc m_cancel;
        config::Diagnostics m_diagnostics{64u};
        std::unique_ptr<ContentAuthoringSession> m_opened{};
        std::unique_ptr<ContentAuthoringSession> m_retired_session{};
        std::shared_ptr<const ContentDescriptorCatalog> m_catalog{};
        std::optional<ContentAuthoringSaveResult> m_saved{};
        std::optional<WorldgenPreviewResult> m_preview{};
        std::shared_ptr<const WorldgenPreviewWorld> m_world{}, m_retired_world{};
        std::unique_ptr<GridPass::GridStagedFrame> m_stage{};
        std::string m_diagnostic_text{};
        std::error_code m_error{};
        bool m_capture_pending{}, m_ready{};
    };

    // ------------------------------------------------------------------
    // owned authored game preparation
    // ------------------------------------------------------------------

    Expected<std::unique_ptr<ColonyDriver>> prepareAuthoredGame(
        const AuthoredGameSource &source, const ColonyDriverDesc &request, const ITimerClock &clock,
        const sim::GenerationExecution &execution, config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Workshop.Start.Prepare"};
        const auto &snapshot = source.m_snapshot;
        auto validated = validateContentDraft(snapshot, diagnostics, execution.m_context.get(), source.m_profile);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, validated);

        auto prepared = prepareWorld(validated->m_selected, source.m_profile, request.m_seed, clock, execution);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, prepared);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, execution.m_context ? execution.m_context->error() : std::error_code{});

        const asset::RequestGeneration generation{request.m_world_generation};
        auto images = ContentPresentationRequest::start(
            prepared->catalogOwner(), generation, execution.m_context, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, images);
        auto ready = images->finish(diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, ready);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, prepared->attachPresentation(std::move(*ready), generation));
        PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, execution.m_context ? execution.m_context->error() : std::error_code{});

        auto driver = std::make_unique<ColonyDriver>();
        const auto error = driver->initPrepared(request, std::move(*prepared), &execution);
        if (error) {
            // Preserve initialization failure while attempting partial driver teardown on this owning worker.
            PPR_LOG_WARNING_ON_FAIL(Workshop, driver->shutdown());
            PPR_RETURN_UNEXPECTED_ON_FAIL(Workshop, error);
        }

        return driver;
    }

    // ------------------------------------------------------------------
    // main-lane ownership and request queue
    // ------------------------------------------------------------------

    WorldgenWorkshop::WorldgenWorkshop(
        ParallelScheduler &pool, const ITimerClock &clock, ParallelScheduler *const generation_pool)
        : m_pool(pool), m_clock(clock), m_generation_pool(generation_pool) {
    }

    WorldgenWorkshop::~WorldgenWorkshop() noexcept {
        if (m_pool.isWorkerThread() or parallel::backgroundPool().isWorkerThread()) {
            hal::fatalExit("workshop-destroyed-on-worker");
        }

        PPR_LOG_WARNING_ON_FAIL(Workshop, shutdown());
    }

    ContentAuthoringSession *WorldgenWorkshop::session() noexcept {
        return m_session.get();
    }

    const WorldgenPreviewResult *WorldgenWorkshop::preview() const noexcept {
        return m_last_preview and m_last_preview->m_preview ? &*m_last_preview->m_preview : nullptr;
    }

    std::shared_ptr<const WorldgenPreviewResult> WorldgenWorkshop::previewHandle() const noexcept {
        return preview() ? std::shared_ptr<const WorldgenPreviewResult>{m_last_preview, preview()} : nullptr;
    }

    std::string_view WorldgenWorkshop::status() const noexcept {
        return m_status.empty() ? std::string_view{"Cancellation requested. Draft and last good preview retained."}
                                : std::string_view{m_status};
    }

    std::string_view WorldgenWorkshop::diagnostics() const noexcept {
        return m_diagnostics;
    }

    std::span<const config::Diagnostic> WorldgenWorkshop::propertyDiagnostics() const noexcept {
        if (not m_session or m_diagnostic_root != m_session->root() or m_diagnostic_revision != m_session->revision()) {
            return {};
        }

        return m_property_diagnostics.entries();
    }

    bool WorldgenWorkshop::draftInvalid() const noexcept {
        return m_session and
               m_invalid_draft_root == m_session->root() and
               m_invalid_draft_revision == m_session->revision();
    }

    bool WorldgenWorkshop::busy() const noexcept {
        return m_running != nullptr or m_replacement != nullptr;
    }

    std::error_code WorldgenWorkshop::queue_(Request request) {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_closing ? std::make_error_code(std::errc::operation_canceled) : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_next_request != 0u ? std::error_code{} : std::make_error_code(std::errc::value_too_large));

        request.m_id = m_next_request++;
        m_latest_request = request.m_id;
        if (m_running) {
            m_running->m_cancel(std::make_error_code(std::errc::operation_canceled));
        }
        m_replacement = std::make_unique<Request>(std::move(request));
        m_status = "Queued; the previous successful preview remains visible.";
        return {};
    }

    std::error_code WorldgenWorkshop::open(std::filesystem::path root, const std::optional<u64> discard_revision) {
        const std::filesystem::path roots[] = {std::move(root)};
        return open(roots, discard_revision);
    }

    std::error_code WorldgenWorkshop::open(
        const std::span<const std::filesystem::path> roots, const std::optional<u64> discard_revision) {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, roots.empty() or std::ranges::any_of(roots, [](const auto &root) { return root.empty(); })
                          ? std::make_error_code(std::errc::invalid_argument)
                          : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, not m_session or not m_session->dirty() or discard_revision == m_session->revision()
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));

        Request request{};
        request.m_kind = Request::Kind::open;
        request.m_roots.assign(roots.begin(), roots.end());
        request.m_path = request.m_roots.front();
        request.m_replaced_revision = m_session ? m_session->revision() : 0u;
        request.m_source_epoch = makeContentSourceEpoch();
        return queue_(std::move(request));
    }

    std::error_code WorldgenWorkshop::validate() {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_session ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        if (explicitOperationPending_()) {
            m_validation_pending = true;
            return {};
        }

        Request request{};
        request.m_kind = Request::Kind::validate;
        request.m_snapshot = m_session->draftSnapshot();

        PPR_RETURN_ERROR_ON_FAIL(Workshop, queue_(std::move(request)));
        m_validation_pending = false;
        return {};
    }

    std::error_code WorldgenWorkshop::requestPreview(WorldgenPreviewSpec spec, const EGridDiagnosticMode mode) {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_session ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, not explicitOperationPending_() ? std::error_code{}
                                                      : std::make_error_code(std::errc::device_or_resource_busy));

        Request request{};
        request.m_kind = Request::Kind::preview;
        request.m_snapshot = m_session->draftSnapshot();
        spec.m_revision = request.m_snapshot->m_revision;
        request.m_spec = spec;
        request.m_mode = mode;

        PPR_RETURN_ERROR_ON_FAIL(Workshop, queue_(std::move(request)));
        m_live_spec = std::move(spec);
        m_live_mode = mode;
        m_live_pending = false;
        m_validation_pending = false;
        return {};
    }

    void WorldgenWorkshop::configureLivePreview(
        const bool enabled, WorldgenPreviewSpec spec, const EGridDiagnosticMode mode) {
        if (not enabled or not m_session or m_closing) {
            m_live_enabled = false;
            m_live_pending = false;
            return;
        }

        spec.m_revision = m_session->revision();
        if (not m_live_enabled or not m_live_spec or *m_live_spec != spec or m_live_mode != mode) {
            m_live_spec = std::move(spec);
            m_live_mode = mode;
            m_live_due = m_clock.now() + std::chrono::milliseconds{350};
            m_live_pending = true;
        }
        m_live_enabled = true;
    }

    std::error_code WorldgenWorkshop::save() {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop,
            m_session and not busy() ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));

        Request request{};
        request.m_kind = Request::Kind::save;
        request.m_snapshot = m_session->draftSnapshot();
        return queue_(std::move(request));
    }

    std::error_code WorldgenWorkshop::exportPreview(std::filesystem::path path, const WorldgenPreviewDisplay display) {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_last_preview and not path.empty() and not busy()
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        Request request{};
        request.m_kind = Request::Kind::export_preview;
        request.m_path = std::move(path);
        request.m_display = display;
        request.m_display.m_view_mode = static_cast<u32>(m_last_preview->m_request.m_mode);
        request.m_export = m_last_preview;
        return queue_(std::move(request));
    }

    void WorldgenWorkshop::cancel() noexcept {
        m_live_pending = false;
        m_validation_pending = false;
        if (m_running) {
            m_running->m_cancel(std::make_error_code(std::errc::operation_canceled));
        }

        if (m_images) {
            m_images->cancel();
        }
        m_replacement.reset();
        ++m_latest_request;
        // Cancellation must reach every owner before any fallible reporting.
        // The query supplies the cancellation message without allocating here.
        m_status.clear();
    }

    void WorldgenWorkshop::close() noexcept {
        m_closing = true;
        cancel();
    }

    std::unique_ptr<GridPass::GridStagedFrame> WorldgenWorkshop::takePresentation() {
        return std::move(m_presentation);
    }

    bool WorldgenWorkshop::explicitOperationPending_() const noexcept {
        const auto explicit_operation = [](const Request::Kind kind) {
            return kind == Request::Kind::open or kind == Request::Kind::save or kind == Request::Kind::export_preview;
        };

        return (m_running and explicit_operation(m_running->m_request.m_kind)) or
               (m_replacement and explicit_operation(m_replacement->m_kind));
    }

    // ------------------------------------------------------------------
    // nonblocking preparation and publication
    // ------------------------------------------------------------------

    std::error_code WorldgenWorkshop::update() {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_pool.isWorkerThread() or parallel::backgroundPool().isWorkerThread()
                          ? std::make_error_code(std::errc::resource_deadlock_would_occur)
                          : std::error_code{});

        const auto matches = [&](const State &state) {
            if (not m_session or
                not state.m_request.m_snapshot or
                state.m_request.m_id != m_latest_request or
                state.m_request.m_snapshot->m_revision != m_session->revision() or
                not state.m_request.m_snapshot->m_source.sameMounts(m_session->draftSource())) {
                return false;
            }

            if (m_live_enabled and m_live_spec and state.m_preview) {
                // Owning copy: worlds() is a span view, so a string_view would
                // dangle against -Wdangling-gsl analysis at this boundary.
                const std::string qualified{state.m_preview->m_catalog->worlds().front().m_key};
                const std::string_view requested = m_live_spec->m_profile;
                const bool world_matches = requested.find(':') != std::string_view::npos
                                               ? requested == qualified
                                               : m_session->draftSource().m_packs.size() == 1u and
                                                     requested == qualified.substr(qualified.find(':') + 1u);

                return world_matches and
                       m_live_spec->m_seed == state.m_request.m_spec.m_seed and
                       m_live_spec->m_biome == state.m_request.m_spec.m_biome and
                       m_live_spec->m_origin == state.m_request.m_spec.m_origin and
                       m_live_spec->m_mode == state.m_request.m_spec.m_mode and
                       m_live_mode == state.m_request.m_mode;
            }

            return true;
        };

        if (m_running and m_images) {
            if (m_closing or not matches(*m_running)) {
                m_images->cancel();
            }

            if (m_images->poll()) {
                auto images = std::move(*m_images);
                m_images.reset();

                // Consumption may throw, including while merging diagnostics.
                // A consumed request must already be terminal in that case.
                m_running->m_ready = true;
                try {
                    auto ready = images.take(m_running->m_diagnostics);
                    if (not ready) {
                        m_running->m_error = ready.error();
                    } else if (m_closing or not matches(*m_running)) {
                        m_running->m_error = std::make_error_code(std::errc::operation_canceled);
                    } else {
                        const auto attached = m_running->m_preview->attachPresentation(
                            std::move(*ready), asset::RequestGeneration{m_running->m_request.m_id});
                        if (attached) {
                            m_running->m_error = attached;
                        } else {
                            m_running->m_capture_pending = true;
                            m_running->m_ready = false;
                        }
                    }

                    m_running->m_diagnostic_text = m_running->m_diagnostics.format();
                } catch (const std::bad_alloc &) {
                    m_running->m_error = std::make_error_code(std::errc::state_not_recoverable);
                    m_running->m_capture_pending = false;
                    m_running->m_ready = true;
                    throw;
                } catch (const std::system_error &exception) {
                    m_running->m_error = exception.code();
                    m_running->m_capture_pending = false;
                    m_running->m_ready = true;
                } catch (...) {
                    m_running->m_error = std::make_error_code(std::errc::state_not_recoverable);
                    m_running->m_capture_pending = false;
                    m_running->m_ready = true;
                }
            }
        }

        if (m_running and m_running->m_capture_pending) {
            if (m_closing or not matches(*m_running)) {
                m_running->m_error = std::make_error_code(std::errc::operation_canceled);
                m_running->m_ready = true;
                m_running->m_capture_pending = false;
            } else {
                const auto state = m_running;

                auto task = m_pool.schedule(
                    [state](const IContext &) -> std::error_code {
                        const hal::ProfileScope scope{"Workshop.CaptureReadyPreview"};
                        auto stage = captureWorldgenPreviewPresentation(
                            *state->m_preview, state->m_request.m_mode, state->m_request.m_id, *state->m_context);
                        PPR_RETURN_ERROR_ON_FAIL(Workshop, stage);

                        state->m_stage = std::make_unique<GridPass::GridStagedFrame>(std::move(*stage));
                        return {};
                    },
                    std::nullopt, ParallelScheduler::drop_if_full);
                if (task) {
                    m_task = std::move(*task);
                    m_running->m_capture_pending = false;
                } else if (task.error() != make_error_code(RawChannel::error_full)) {
                    m_running->m_error = task.error();
                    m_running->m_ready = true;
                    m_running->m_capture_pending = false;
                }
            }
        }

        if (m_running and (m_running->m_ready or (m_task.valid() and m_task.wait_for(std::chrono::seconds{0}) ==
                                                                         std::future_status::ready))) {
            const auto state = m_running;
            auto error = state->m_error;
            if (m_task.valid()) {
                state->m_ready = true;
                try {
                    error = m_task.get();
                } catch (const std::bad_alloc &) {
                    state->m_error = std::make_error_code(std::errc::state_not_recoverable);
                    throw;
                } catch (const std::system_error &exception) {
                    error = exception.code();
                } catch (...) {
                    error = std::make_error_code(std::errc::state_not_recoverable);
                }
            }

            // Numerical workers never create or wait for image requests. This
            // external owner admits/polls images before dispatching CPU capture.
            if (not error and state->m_preview and not state->m_preview->m_presentation) {
                if (m_closing or not matches(*state)) {
                    error = std::make_error_code(std::errc::operation_canceled);
                } else {
                    auto images = ContentPresentationRequest::start(
                        state->m_preview->m_catalog, asset::RequestGeneration{state->m_request.m_id}, state->m_context,
                        state->m_diagnostics, state->m_request.m_snapshot->m_source_epoch,
                        m_last_preview and m_last_preview->m_preview ? m_last_preview->m_preview->m_presentation
                                                                     : nullptr);
                    if (images) {
                        m_images.emplace(std::move(*images));
                        state->m_ready = false;
                        return {};
                    }

                    error = images.error();
                    state->m_diagnostic_text = state->m_diagnostics.format();
                }
            }

            m_completed = std::move(m_running);

            // An atomic replacement may have committed before cancellation.
            // Always advance its matching session baseline, preserving newer edits.
            if (state->m_saved and m_session and state->m_saved->m_snapshot.m_root == m_session->root()) {
                (void) m_session->acceptSave(std::move(*state->m_saved));
            }

            if (error == std::errc::operation_canceled and not m_replacement and not m_closing) {
                m_status = "Cancelled. Draft and last good preview retained.";
            }

            if (state->m_request.m_id == m_latest_request and not m_closing) {
                m_diagnostics = state->m_diagnostic_text;
                const bool diagnostic_matches = m_session and
                                                state->m_request.m_snapshot and
                                                state->m_request.m_snapshot->m_root == m_session->root() and
                                                state->m_request.m_snapshot->m_source.sameMounts(
                                                    m_session->draftSource()) and
                                                state->m_request.m_snapshot->m_revision == m_session->revision();
                if (diagnostic_matches and
                    error != std::errc::operation_canceled and
                    state->m_request.m_kind != Request::Kind::export_preview) {
                    m_property_diagnostics = state->m_diagnostics;
                    m_diagnostic_root = m_session->root();
                    m_diagnostic_revision = m_session->revision();
                }

                // Operational and rejected-edit errors do not invalidate source.
                // Only a completed catalog validation controls preview admission.
                if (diagnostic_matches and
                    error != std::errc::operation_canceled and
                    (state->m_request.m_kind == Request::Kind::validate or
                        state->m_request.m_kind == Request::Kind::preview)) {
                    if (not state->m_catalog and state->m_diagnostics.hasErrors()) {
                        m_invalid_draft_root = m_session->root();
                        m_invalid_draft_revision = m_session->revision();
                    } else if (state->m_catalog) {
                        m_invalid_draft_root.clear();
                        m_invalid_draft_revision = 0u;
                    }
                }

                if (error == std::errc::operation_canceled) {
                    m_status = "Cancelled. Draft and last good preview retained.";
                } else if (error) {
                    constexpr std::string_view operations[]{
                        "Open pack", "Validate draft", "Generate preview", "Save pack", "Export preview"};
                    m_status = std::string{operations[static_cast<std::size_t>(state->m_request.m_kind)]} +
                               " failed: " +
                               error.message() +
                               ". Draft and last good preview retained.";
                    if (state->m_diagnostics.hasErrors()) {
                        const auto &first = state->m_diagnostics.entries().front();
                        m_status += "\n" + first.m_property + ": " + first.m_message;
                    } else if (state->m_request.m_kind == Request::Kind::open or
                               state->m_request.m_kind == Request::Kind::save) {
                        const auto &path = state->m_request.m_snapshot ? state->m_request.m_snapshot->m_root
                                                                       : state->m_request.m_path;
                        m_status += "\nPack root: " + path.string();
                    }
                    if (state->m_request.m_kind == Request::Kind::export_preview) {
                        m_status += " Partial output may remain at " + state->m_request.m_path.string();
                    }
                } else {
                    if (state->m_opened and
                        (m_session ? m_session->revision() : 0u) != state->m_request.m_replaced_revision) {
                        m_status =
                            "Open cancelled because the draft changed while loading. Draft and history retained.";
                        return {};
                    }
                    if (state->m_opened) {
                        state->m_retired_session = std::move(m_session);
                        state->m_retired_world = std::move(m_world_cache);
                        m_live_spec.reset();
                        m_live_pending = false;
                        m_session = std::move(state->m_opened);
                        m_property_diagnostics.clear();
                        m_diagnostic_root.clear();
                        m_diagnostic_revision = 0u;
                        m_invalid_draft_root.clear();
                        m_invalid_draft_revision = 0u;
                    }
                    if (m_session and state->m_catalog and state->m_request.m_snapshot) {
                        (void) m_session->acceptValidation(state->m_request.m_snapshot->m_revision, state->m_catalog);
                    }
                    const bool matching = matches(*state);
                    if (state->m_preview and matching) {
                        if (state->m_world) {
                            m_world_cache = state->m_world;
                        }
                        m_retired_preview = std::move(m_last_preview);
                        m_last_preview = state;
                        m_presentation = std::move(state->m_stage);
                    }
                    m_status = state->m_request.m_kind == Request::Kind::export_preview
                                   ? "Preview exported to " + state->m_request.m_path.string()
                                   : "Request completed.";
                }
            }
        }

        // Explicit I/O owns its snapshot until completion. Live edits may replace
        // numerical work, but must never cancel a save, open, or export.
        if (m_validation_pending and not m_closing and not explicitOperationPending_()) {
            PPR_RETURN_ERROR_ON_FAIL(Workshop, validate());
        }

        if (m_live_pending and
            m_live_spec and
            m_live_enabled and
            not m_closing and
            not explicitOperationPending_() and
            m_clock.now() >= m_live_due) {
            if (draftInvalid()) {
                m_live_pending = false;
                m_status = "Live preview paused: fix the highlighted properties. Your last good preview is retained.";
            } else {
                PPR_RETURN_ERROR_ON_FAIL(Workshop, requestPreview(*m_live_spec, m_live_mode));
            }
        }

        if (m_running or not m_replacement or m_closing) {
            return {};
        }

        auto state = std::make_shared<State>();
        state->m_request = *m_replacement;
        state->m_world = m_world_cache;
        std::tie(state->m_context, state->m_cancel) = context::withCancelClause(context::background());

        // Retain the previous completed task until a worker owns its final disposal.
        const auto retired = m_completed;
        const auto retired_preview = m_retired_preview;
        auto task = m_pool.schedule(
            [state, retired, retired_preview, clock = &m_clock, generation_pool = m_generation_pool](
                const IContext &) -> std::error_code {
                auto execute = [&]() -> std::error_code {
                    PPR_RETURN_ERROR_ON_FAIL(Workshop, state->m_context->error());
                    auto &request = state->m_request;

                    if (request.m_kind == Request::Kind::open) {
                        auto opened = ContentAuthoringSession::open(
                            request.m_roots, state->m_diagnostics, state->m_context.get(), kWorldCompileParameters,
                            request.m_source_epoch);
                        PPR_RETURN_ERROR_ON_FAIL(Workshop, opened);
                        state->m_opened = std::make_unique<ContentAuthoringSession>(std::move(*opened));
                    } else if (request.m_kind == Request::Kind::save) {
                        auto saved = saveContentDraft(
                            *request.m_snapshot, state->m_diagnostics, state->m_context.get());
                        PPR_RETURN_ERROR_ON_FAIL(Workshop, saved);

                        state->m_saved = std::move(*saved);
                        PPR_RETURN_ERROR_ON_FAIL(Workshop, state->m_saved->m_error);
                    } else if (request.m_kind == Request::Kind::export_preview) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            Workshop,
                            exportWorldgenPreview(
                                *request.m_export->m_preview, request.m_export->m_request.m_snapshot->m_source,
                                request.m_display, request.m_path, state->m_context.get()));
                    } else {
                        auto catalog = validateContentDraft(
                            *request.m_snapshot, state->m_diagnostics, state->m_context.get(),
                            request.m_kind == Request::Kind::preview
                                ? std::optional<std::string_view>{request.m_spec.m_profile}
                                : std::nullopt);
                        PPR_RETURN_ERROR_ON_FAIL(Workshop, catalog);

                        state->m_catalog = catalog->m_definitions;

                        if (request.m_kind == Request::Kind::preview) {
                            const auto selected = catalog->m_selected;
                            request.m_spec.m_profile = selected->worlds().front().m_key;

                            if (request.m_spec.m_mode == EWorldgenPreviewMode::whole_world_crop) {
                                if (not state->m_world or not state->m_world->matches(*selected, request.m_spec)) {
                                    state->m_world.reset();
                                    auto world = prepareWorldgenPreviewWorld(
                                        *selected, request.m_spec, *clock, state->m_context, generation_pool);
                                    PPR_RETURN_ERROR_ON_FAIL(Workshop, world);
                                    state->m_world = std::move(*world);
                                }
                            } else {
                                state->m_world.reset();
                            }

                            auto preview = prepareWorldgenPreview(
                                selected, request.m_spec, state->m_context, state->m_world);
                            PPR_RETURN_ERROR_ON_FAIL(Workshop, preview);

                            state->m_preview = std::move(*preview);
                        }
                    }

                    PPR_RETURN_ERROR_ON_FAIL(Workshop, state->m_context->error());
                    return {};
                };

                std::error_code error{};
                try {
                    error = execute();
                } catch (const std::bad_alloc &) {
                    throw;
                } catch (const std::system_error &exception) {
                    error = exception.code();
                } catch (...) {
                    error = std::make_error_code(std::errc::state_not_recoverable);
                }

                state->m_diagnostic_text = state->m_diagnostics.format();
                if (state->m_diagnostic_text.size() > 64u * 1024u) {
                    state->m_diagnostic_text.resize(64u * 1024u);
                }

                return error;
            },
            std::nullopt, ParallelScheduler::drop_if_full);

        if (not task and task.error() == make_error_code(RawChannel::error_full)) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(Workshop, task);

        m_completed.reset();
        m_retired_preview.reset();
        m_task = std::move(*task);
        m_running = std::move(state);
        m_replacement.reset();
        m_status = "Working; edits can queue one replacement snapshot.";
        return {};
    }

    // ------------------------------------------------------------------
    // external-owner teardown
    // ------------------------------------------------------------------

    std::error_code WorldgenWorkshop::shutdown() {
        PPR_RETURN_ERROR_ON_FAIL(
            Workshop, m_pool.isWorkerThread() or parallel::backgroundPool().isWorkerThread()
                          ? std::make_error_code(std::errc::resource_deadlock_would_occur)
                          : std::error_code{});

        close();
        std::error_code error{};
        std::exception_ptr fatal;
        const auto retain = [&](const std::error_code failure) {
            // Reporting waits until every independent owner has been drained.
            if (failure and failure != std::errc::operation_canceled and not error) {
                error = failure;
            }
        };

        if (m_images) {
            auto images = std::move(*m_images);
            m_images.reset();
            config::Diagnostics diagnostics;
            try {
                auto settled = images.finish(diagnostics);
                if (not settled) {
                    retain(settled.error());
                }
            } catch (const std::bad_alloc &) {
                fatal = std::current_exception();
            } catch (const std::system_error &exception) {
                retain(exception.code());
            } catch (...) {
                retain(std::make_error_code(std::errc::state_not_recoverable));
            }
        }

        if (m_task.valid()) {
            try {
                const hal::ProfileScope scope{"Workshop.Drain", hal::profile_blocking_category};
                retain(m_task.get());
            } catch (const std::bad_alloc &) {
                if (not fatal) {
                    fatal = std::current_exception();
                }
            } catch (const std::system_error &exception) {
                retain(exception.code());
            } catch (...) {
                retain(std::make_error_code(std::errc::state_not_recoverable));
            }
        }

        m_running.reset();
        m_completed.reset();
        m_last_preview.reset();
        m_retired_preview.reset();
        m_presentation.reset();
        m_world_cache.reset();
        m_live_spec.reset();
        m_session.reset();

        if (fatal) {
            std::rethrow_exception(fatal);
        }

        PPR_RETURN_ERROR_ON_FAIL(Workshop, error);
        return {};
    }
}
