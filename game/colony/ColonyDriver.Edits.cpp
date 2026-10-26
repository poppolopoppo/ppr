module;
#include "pP/Macros.h"
module game.colony.driver;
import game.colony.digtool;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyEdits, info, none)

    // ------------------------------------------------------------------
    // edit publication
    // ------------------------------------------------------------------

    void ColonyDriver::requireEditPublication() noexcept {
        m_require_edit_publication = true;
    }

    bool ColonyDriver::editPending() const noexcept {
        return m_edit.has_value();
    }

    bool ColonyDriver::editReadyForPresentation() const noexcept {
        return m_edit and m_edit->m_colliders_ready and (not m_edit->m_matter or m_edit->m_matter_copied);
    }

    std::span<const sim::ChunkGrid::CapturedChunk> ColonyDriver::editChunks() const noexcept {
        return editReadyForPresentation() ? std::span<const sim::ChunkGrid::CapturedChunk>{m_edit->m_chunks}
                                          : std::span<const sim::ChunkGrid::CapturedChunk>{};
    }

    std::span<const ColonyDriver::DirtyChunk> ColonyDriver::editManifest() const noexcept {
        return editReadyForPresentation() ? std::span<const DirtyChunk>{m_edit->m_manifest}
                                          : std::span<const DirtyChunk>{};
    }

    // ------------------------------------------------------------------
    // held ticks
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::runHeldTicks() {
        while (m_held_slices != 0u) {
            if (m_matter_pending) {
                const auto finished = finishMatterPhysics();
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, finished);
                if (not *finished) {
                    return {};
                }
            } else {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_steps.runSystems(m_registry));
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_step_error);
                if (m_matter_pending or m_snapshot_phase != ESnapshotPhase::idle) {
                    return {};
                }
            }

            if (m_edit) {
                m_resume_edit_tick = true;
                // Prefix stopped before search/planning/movement. Retain this
                // slice and every already-admitted catch-up slice exactly once.
                return {};
            }

            m_resume_edit_tick = false;
            --m_held_slices;
            ++m_tick_count;
            m_counts = pP::colony::pathCounts(m_registry);
            m_errands = liveErrandCount(m_registry);
            refreshAgents();

            if (m_pending_single_steps != 0u) {
                --m_pending_single_steps;
                return {};
            }

            if (m_paused) {
                // Finish the already-admitted in-flight slice, then retain all
                // later catch-up slices until an explicit resume/single-step.
                return {};
            }
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit preparation
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::prepareEdit(const bool advance_build) {
        if (m_matter_runtime != nullptr and
            not m_cpu_particles_current and
            (not m_pending_digs.empty() or (advance_build and liveErrandCount(m_registry) != 0u))) {
            const auto captured = advanceMatterSnapshot();
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, captured);
            if (not *captured) {
                return {};
            }
        }

        if (m_matter_runtime != nullptr and
            contentCatalog() == nullptr and
            (not m_pending_digs.empty() or (advance_build and liveErrandCount(m_registry) != 0u))) {
            // Legacy fixtures have no authoritative emission density.
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::not_supported);
        }

        EditCandidate candidate{};
        auto capture = [&](const sim::ChunkPos pos) -> std::error_code {
            if (std::ranges::any_of(candidate.m_chunks, [pos](const auto &chunk) { return chunk.position() == pos; })) {
                return {};
            }
            auto chunk = grid().captureChunk(pos);
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, chunk);
            candidate.m_chunks.push_back(std::move(*chunk));
            return {};
        };

        if (advance_build) {
            for (const auto &[entity, errand]: m_registry.view<BuildErrand>()) {
                if (errand.m_progress < errand.m_total) {
                    candidate.m_errands.push_back({entity, errand.m_progress, errand.m_progress});
                }
            }
        }

        bool restore = true;
        PPR_DEFER {
            // Includes errors and fatal allocation unwinding. No partially
            // prepared CPU cells or errand progress escape this exclusive call.
            if (restore) {
                for (auto &chunk: candidate.m_chunks) {
                    grid().exchangeChunk(chunk);
                }
                for (const auto &row: candidate.m_errands) {
                    m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_original;
                }
            }
        };

        if (not m_pending_digs.empty()) {
            const auto &dig = m_pending_digs.front();
            candidate.m_has_dig = true;
            for (u32 y = dig.m_min.m_y / sim::kChunkEdge; y <= (dig.m_min.m_y + dig.m_height - 1u) / sim::kChunkEdge;
                ++y) {
                for (u32 x = dig.m_min.m_x / sim::kChunkEdge; x <= (dig.m_min.m_x + dig.m_width - 1u) / sim::kChunkEdge;
                    ++x) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, capture({x, y}));
                }
            }
            const auto dug = digCells(grid(), {dig.m_min, dig.m_width, dig.m_height}, contentCatalog());
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, dug);
            candidate.m_dug = dug->m_dug;
            candidate.m_touched = dug->m_touched;
        }

        if (advance_build) {
            Pathfinder scratch{};
            // Capture only chunks actually written, bounded by this tick's
            // 64-cell budget, regardless of the number of queued errands.
            PPR_RETURN_ERROR_ON_FAIL(
                ColonyEdits, advanceErrands(
                                 grid(), m_registry, scratch, candidate.m_touched, kErrandCellsPerTick,
                                 contentCatalog(), std23::function_ref<std::error_code(sim::ChunkPos)>{capture}));
        }

        for (auto &row: candidate.m_errands) {
            row.m_candidate = m_registry.get<BuildErrand>(row.m_entity)->m_progress;
            m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_original;
        }

        for (auto &chunk: candidate.m_chunks) {
            grid().exchangeChunk(chunk);
        }

        restore = false;
        if (candidate.m_touched.empty()) {
            for (const auto &row: candidate.m_errands) {
                m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_candidate;
            }
            if (candidate.m_has_dig) {
                m_pending_digs.erase(m_pending_digs.begin());
            }
            return {};
        }

        std::erase_if(candidate.m_chunks, [&](const auto &chunk) {
            return std::ranges::find(candidate.m_touched, chunk.position()) == candidate.m_touched.end();
        });

        if (m_matter_runtime != nullptr) {
            auto matter = prepareMatterTerrainEdit(*m_matter, grid(), candidate.m_chunks, *contentCatalog());
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, matter);
            if (not matter->has_value()) {
                // Expected coverage deferral retains the whole tool cohort.
                // Continue physics on committed terrain; retry on a later tick.
                m_edit_rejection = std::make_error_code(std::errc::device_or_resource_busy);
                return {};
            }

            auto &working = **matter;
            // An edit does not integrate position. Preserve the current physical
            // ledger exactly instead of reconstructing it through FP32 readback.
            std::array<Array<sim::MatterParticle>, sim::kChunkCount> rows{};
            for (const auto records:
                {std::span<const sim::MatterParticle>{working.m_records},
                    std::span<const sim::MatterParticle>{working.m_gas}}) {
                for (const auto &record: records) {
                    const sim::GlobalCellPos position{
                        std::min(static_cast<u32>(record.m_x_m / working.m_cell_metres), sim::kWorldEdge - 1u),
                        std::min(static_cast<u32>(record.m_y_m / working.m_cell_metres), sim::kWorldEdge - 1u)};
                    rows[sim::chunkIndexOf(sim::chunkOf(position))].push_back(record);
                }
            }

            sim::MatterParticleSnapshot snapshot{.m_next_id = working.m_next_id};
            for (u32 index = 0u; index < sim::kChunkCount; ++index) {
                if (rows[index].empty())
                    continue;

                std::ranges::sort(rows[index], {}, &sim::MatterParticle::m_id);
                snapshot.m_chunks.push_back(
                    {{index % sim::kChunksPerEdge, index / sim::kChunksPerEdge},
                        std::make_shared<const Array<sim::MatterParticle>>(std::move(rows[index]))});
            }

            PPR_RETURN_ERROR_ON_FAIL(
                ColonyEdits, candidate.m_moving_particles.restoreMoving(
                                 snapshot, preparedWorld()->generated().m_plan.recipe().m_materials,
                                 working.m_cell_metres, working.m_thickness_metres));

            candidate.m_matter = std::move(working);
        } else {
            auto particles = m_colony.particles().prepareTerrainEdit(grid(), candidate.m_chunks, 1.0 / cellsPerMetre());
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, particles);
            candidate.m_particles = std::move(*particles);
        }

        for (const auto pos: candidate.m_touched) {
            u64 revision = 1u;
            for (const auto &row: m_present_revision_history) {
                if (row.m_pos == pos) {
                    revision = row.m_revision + 1u;
                    break;
                }
            }
            PPR_RETURN_ERROR_ON_FAIL(
                ColonyEdits, revision != 0u ? std::error_code{} : std::make_error_code(std::errc::value_too_large));
            candidate.m_manifest.push_back({pos, revision});
        }

        m_edit.emplace(std::move(candidate));
        // The admitted command belongs to the candidate, so later bounded
        // FIFO overflow cannot evict it or make settlement pop another dig.
        if (m_edit->m_has_dig) {
            m_pending_digs.erase(m_pending_digs.begin());
        }

        invalidateColliderCache();
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, prepareEditColliders());

        if (not m_edit)
            return {}; // Attached simulation settlement has no render prerequisite.

        if (not m_require_edit_publication) {
            // Headless authored sessions publish the same prepared transaction
            // synchronously, without depending on a render acknowledgement.
            while (m_edit and not editReadyForPresentation()) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, prepareEditColliders());
            }

            if (m_edit)
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, settleEdit(m_edit->m_manifest));
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit colliders
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::prepareEditColliders() {
        PPR_ASSERT(m_edit.has_value());
        if (m_edit->m_colliders_ready and not m_edit->m_matter) {
            return {};
        }

        if (not m_edit->m_colliders_ready) {
            Array<sim::ChunkPos> needed{};
            collectColliderInterest(needed);

            if (not m_edit->m_colliders_started) {
                if (m_colliders.pendingCount() != 0u) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, rebuildChunks({}, needed, 2u));
                    if (m_colliders.pendingCount() != 0u) {
                        return {};
                    }
                }

                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_colliders.beginCohort(m_scene));
                m_edit->m_colliders_started = true;

                // Candidate cells are exposed only inside this main-thread call;
                // cached collider views own their numeric copies. UI and rendering
                // keep the original grid throughout every inter-frame wait.
                for (auto &chunk: m_edit->m_chunks) {
                    grid().exchangeChunk(chunk);
                }
                PPR_DEFER {
                    for (auto &chunk: m_edit->m_chunks) {
                        grid().exchangeChunk(chunk);
                    }
                };

                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, rebuildChunks(m_edit->m_touched, needed, 2u));
            } else {
                // The frozen candidate cache already owns the complete neighbor closure.
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, rebuildChunks({}, needed, 2u));
            }

            m_edit->m_colliders_ready = m_colliders.pendingCount() == 0u;
        }

        if (m_edit->m_colliders_ready and m_edit->m_matter) {
            // Reserve all bookkeeping before the first irreversible publication.
            m_present_dirty.reserve(m_present_dirty.size() + m_edit->m_manifest.size());
            m_present_revision_history.reserve(m_present_revision_history.size() + m_edit->m_manifest.size());

            if (m_edit->m_matter_prepared) {
                if (not m_edit->m_matter_copied) {
                    auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::edit_poll, [&] {
                        return m_matter_runtime->beginPollEdit(m_edit->m_matter->m_identity);
                    });
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, completion);
                    if (not *completion) {
                        return {};
                    }

                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_operation_error);
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);
                    if (not (**completion).m_readback) {
                        return {};
                    }

                    m_edit->m_matter_copied = true;
                }

                return settleEdit(m_edit->m_manifest);
            }

            auto completion = advanceMatterRequest(
                *m_matter_runtime, EMatterOperation::edit_prepare, [&]() -> Expected<MatterRequest> {
                    Array<u16> solids;
                    for (const auto &material: contentCatalog()->materials()) {
                        if (material.m_representation == EMaterialRepresentation::terrain and
                            material.m_phase == EMaterialPhase::solid) {
                            solids.push_back(material.m_numeric_id);
                        }
                    }

                    for (auto &chunk: m_edit->m_chunks) {
                        grid().exchangeChunk(chunk);
                    }

                    auto terrain = [&] {
                        PPR_DEFER {
                            for (auto &chunk: m_edit->m_chunks) {
                                grid().exchangeChunk(chunk);
                            }
                        };

                        return makeMatterTerrain(grid(), solids);
                    }();
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, terrain);
                    return m_matter_runtime->beginPrepareEdit(
                        m_matter->m_identity, *m_edit->m_matter, m_matter->m_profile.m_particle_count,
                        std::move(*terrain));
                });
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, completion);
            if (not *completion) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_operation_error);
            m_edit->m_matter_prepared = true;
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);

            // Acknowledged upload submission is distinct from its copy fence.
            // The next poll alone can admit joint publication.
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit settlement
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::settleEdit(const std::span<const DirtyChunk> manifest) {
        if (not editReadyForPresentation()) {
            return {};
        }
        const bool matches = std::ranges::all_of(m_edit->m_manifest, [&](const DirtyChunk &expected) {
            return std::ranges::any_of(manifest, [&](const DirtyChunk &row) {
                return row.m_pos == expected.m_pos and row.m_revision == expected.m_revision;
            });
        });
        if (not matches) {
            return {};
        }

        if (m_edit->m_matter) {
            PPR_ASSERT(m_matter_runtime != nullptr and m_edit->m_matter_prepared);
            // Prepared CPU/GPU state and collider cohort are frozen together.
            // Either publication may be irreversible even on an unexpected error.
            auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::edit_publish, [&] {
                return m_matter_runtime->beginPublishEdit(m_edit->m_matter->m_identity);
            });
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, completion);
            if (not *completion) {
                return {};
            }

            if ((**completion).m_execution.m_operation_error or m_colliders.commitCohort(m_scene)) {
                hal::fatalExit("matter-terrain-joint-publication-failure");
            }

            m_edit->m_colliders_started = false;
            m_edit->m_matter_prepared = false;

            for (auto &chunk: m_edit->m_chunks)
                grid().exchangeChunk(chunk);

            m_matter = std::move(m_edit->m_matter);
            m_colony.particles() = std::move(m_edit->m_moving_particles);
            const auto presentation_world = m_particle_view->m_world;
            m_particle_view = (**completion).m_view;
            PPR_ASSERT(m_particle_view.has_value());
            m_particle_view->m_world = presentation_world;
            m_particle_view->m_joint_tick = m_tick_count;
            m_cpu_particles_current = true;
            m_snapshot_tick = m_tick_count;
            m_capture_awake_valid = false;

            for (const auto &row: m_edit->m_errands)
                m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_candidate;

            for (const auto &row: m_edit->m_manifest) {
                auto present = std::ranges::find(m_present_dirty, row.m_pos, &DirtyChunk::m_pos);
                if (present == m_present_dirty.end())
                    m_present_dirty.push_back(row);
                else
                    *present = row;

                auto history = std::ranges::find(m_present_revision_history, row.m_pos, &DirtyChunk::m_pos);
                if (history == m_present_revision_history.end())
                    m_present_revision_history.push_back(row);
                else
                    *history = row;
            }

            m_collider_dirty.clear();
            m_finder = Pathfinder{};
            m_dig_stats.m_dug_total += m_edit->m_dug;
            m_initial_matter_baseline_valid = false;
            invalidateColliderCache();

            if (wakeBodiesNearEdit(m_edit->m_touched))
                hal::fatalExit("matter-terrain-postcommit-wake-failure");

            m_dig_stats.m_last_chunks = std::move(m_edit->m_touched);
            m_edit.reset();
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_colliders.commitCohort(m_scene));
        m_edit->m_colliders_started = false;

        for (auto &chunk: m_edit->m_chunks) {
            grid().exchangeChunk(chunk);
        }

        m_colony.particles().commitTerrainEdit(m_edit->m_particles);

        for (const auto &row: m_edit->m_errands) {
            m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_candidate;
        }

        fanOutEditChunks(m_edit->m_touched);
        m_collider_dirty.clear(); // Already prepared and committed as one cohort.
        m_finder = Pathfinder{};
        m_dig_stats.m_dug_total += m_edit->m_dug;

        const auto wake_error = wakeBodiesNearEdit(m_edit->m_touched);
        // The transaction is committed even if the postcommit physics wake fails.
        m_edit.reset();
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, wake_error);
        return {};
    }
}
