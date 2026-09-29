#pragma once

// In-tick stage wall-clock timings (Slice 6 diagnostics).
//
// Pure observation side-channel: accumulated values NEVER enter sim state,
// the registry, snapshots, or determinism paths. Per AGENTS.md, time is an
// injected dependency and lower-level logic must not read an implicit clock
// as sim input; these steady_clock reads are boundary diagnostics only
// (~ns each, always on by design, no flags), measuring work the tick already
// performs without altering its behavior.
//
// Counter rows (CacheHit, CacheMiss, NeededCells, ChunksDrained) record
// calls only (0us always): a 0.00 mean => counter row, not a timed stage.
// NeededCells sums sorted_needed.size() per rebuildChunks call, so its mean
// is the average needed-set size per tick. ChunksDrained sums the
// rebuildDirtyChunks `processed` out-param per drain (callee-returned
// count, not a pending-before/after delta). TimeTrials CSV loops
// Stage::Count, so new rows appear with zero reader changes.
//
// Plain header (not a module), following the Colony.Elements.h precedent:
// included from a module global fragment without taking a module
// dependency. Uses <array>, <chrono>, and <cstdint> only; all inline and
// noexcept, no allocations, no logging.

#include <array>
#include <chrono>
#include <cstdint>

namespace pP::colony {
    enum class Stage : std::uint8_t {
        Tool,
        Build,
        Pathfind,
        Plan,
        Move,
        CollidersPrepare,
        CollidersRebuild,
        PhysicsStep,
        Mirror,
        Summary,
        Invalidate,
        Submit,
        Tick,
        Update,
        CacheHit,
        CacheMiss,
        NeededCells,
        ChunksDrained,
        Count
    };

    struct StageStat {
        std::uint64_t m_microseconds{};
        std::uint64_t m_calls{};
        // Worst single scope entry (us) in the window; resets with the table.
        std::uint64_t m_max_us{};
    };

    struct StageTimings {
        std::array<StageStat, (std::size_t)Stage::Count> m_stats{};
    };

    [[nodiscard]] inline const char* stageName(Stage s) noexcept {
        switch (s) {
            case Stage::Tool: return "Colony.Tool";
            case Stage::Build: return "Colony.Build";
            case Stage::Pathfind: return "Colony.Pathfind";
            case Stage::Plan: return "Colony.Plan";
            case Stage::Move: return "Colony.Move";
            case Stage::CollidersPrepare: return "Colony.Colliders.Prepare";
            case Stage::CollidersRebuild: return "Colony.Colliders.Rebuild";
            case Stage::PhysicsStep: return "Colony.Physics.Step";
            case Stage::Mirror: return "Colony.Physics.Mirror";
            case Stage::Summary: return "Colony.Summary";
            case Stage::Invalidate: return "Colony.Presentation.Invalidate";
            case Stage::Submit: return "Colony.Submit";
            case Stage::Tick: return "Colony.Tick";
            case Stage::Update: return "Colony.Update";
            case Stage::CacheHit: return "Colony.Colliders.CacheHit";
            case Stage::CacheMiss: return "Colony.Colliders.CacheMiss";
            case Stage::NeededCells: return "Colony.Colliders.NeededCells";
            case Stage::ChunksDrained: return "Colony.Colliders.ChunksDrained";
            default: return "Colony.Unknown";
        }
    }

    class StageTimer {
    public:
        explicit StageTimer(StageTimings& t, Stage s) noexcept
            : m_t(&t), m_s(s), m_t0(std::chrono::steady_clock::now()) {}

        ~StageTimer() noexcept {
            const auto elapsed = std::chrono::steady_clock::now() - m_t0;
            StageStat& stat = m_t->m_stats[(std::size_t)m_s];
            const auto single =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
            stat.m_microseconds += single;
            stat.m_calls += 1u;
            if (single > stat.m_max_us) {
                stat.m_max_us = single;
            }
        }

        StageTimer(const StageTimer&) = delete;
        StageTimer& operator=(const StageTimer&) = delete;
        StageTimer(StageTimer&&) = delete;
        StageTimer& operator=(StageTimer&&) = delete;

    private:
        StageTimings* m_t;
        Stage m_s;
        std::chrono::steady_clock::time_point m_t0;
    };
} // namespace pP::colony
