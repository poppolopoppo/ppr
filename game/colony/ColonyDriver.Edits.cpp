module;
#include "pP/Macros.h"
module game.colony.driver;
import game.colony.digtool;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyEdits, info, none);

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
        return m_edit and m_edit->m_colliders_ready;
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
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_steps.runSystems(m_registry));
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_step_error);

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
            if (m_single_step_pending) {
                m_single_step_pending = false;
                return {};
            }
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit preparation
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::prepareEdit(const bool advance_build) {
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

        auto particles = m_colony.particles().prepareTerrainEdit(grid(), candidate.m_chunks, 1.0 / cellsPerMetre());
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, particles);
        candidate.m_particles = std::move(*particles);

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

        if (not m_require_edit_publication) {
            // Headless authored sessions publish the same prepared transaction
            // synchronously, without depending on a render acknowledgement.
            while (not editReadyForPresentation()) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, prepareEditColliders());
            }
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, settleEdit(m_edit->m_manifest));
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit colliders
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::prepareEditColliders() {
        PPR_ASSERT(m_edit.has_value());
        if (m_edit->m_colliders_ready) {
            return {};
        }

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
