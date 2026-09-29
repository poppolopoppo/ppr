module;
#include "StageTiming.h"
#include "Colony.Elements.h"

module game.colony.timetrials;

import engine.core;
import engine.sim;
import game.colony.buildings;
import game.colony.driver;
import std;

namespace pP::colony {
    namespace {
        constexpr u64 kTrialSeed = 1234567u;
        constexpr u32 kTrialAgents = 100u;
        constexpr u32 kTrialTicks = 600u;
        constexpr u32 kWallPeriod = 30u;

        // Alternating build/demolish pairs on rotating rects: period p issues
        // build (p even) or demolish (p odd) on rect[(p / 2) % 4], so each
        // pair raises a wall then levels it. A deterministic grid probe flips
        // the op when the base op would write nothing (all-target rect), so
        // EVERY period performs real cell changes; errands are 16 cells
        // against a 64-cell tick budget, so each drains within its period.
        // Fixed 8x2 rock rects, rotating every two periods. Hardcoded
        // seed-derived positions: no RNG at runtime, so the schedule is a
        // pure function of the tick plus the probed grid state (itself a pure
        // function of the seed and the schedule).
        constexpr sim::GlobalCellPos kWallRects[4] = {
            {2000u, 2000u},
            {2016u, 2000u},
            {2000u, 2016u},
            {2016u, 2016u},
        };
        constexpr u32 kWallWidth = 8u;
        constexpr u32 kWallHeight = 2u;

        void printTimingsCsv(const StageTimings &timings) {
            std::println("stage,calls,total_us,mean_us,max_us");
            for (std::size_t index = 0u; index < static_cast<std::size_t>(Stage::Count); ++index) {
                const Stage stage = static_cast<Stage>(index);
                const StageStat &stat = timings.m_stats[index];
                const u64 calls = stat.m_calls;
                const u64 total = stat.m_microseconds;
                const double mean = calls > 0u ? static_cast<double>(total) / static_cast<double>(calls) : 0.0;
                // NOTE: std::println with >2 fixed-width args trips consteval
                // _Format_checker C3546 on this toolchain (VS18 Insiders);
                // printf is runtime-parsed and prints byte-identical CSV.
                std::printf("%s,%llu,%llu,%.2f,%llu\n", stageName(stage), static_cast<unsigned long long>(calls),
                    static_cast<unsigned long long>(total), static_cast<double>(mean),
                    static_cast<unsigned long long>(stat.m_max_us));
            }
        }

        [[nodiscard]] Expected<void> runSteady() {
            ColonyDriver driver{};
            if (const std::error_code error =
                    driver.init(ColonyDriverDesc{.m_seed = kTrialSeed, .m_agent_count = kTrialAgents})) {
                return std::unexpected{error};
            }
            driver.setPaused(true);
            driver.resetStageTimings();

            // Phase: step. Fixed tick count, no wall-clock in the workload.
            std::error_code first_error{};
            for (u32 tick = 0u; tick < kTrialTicks and not first_error; ++tick) {
                first_error = driver.stepOne();
                if (not first_error) {
                    if (tick + 1u == 300u) {
                        std::fprintf(stderr, "[timetrials] S1 [segment 0-300]\n");
                        printTimingsCsv(driver.stageTimings());
                        driver.resetStageTimings();
                    } else if (tick + 1u == 500u) {
                        std::fprintf(stderr, "[timetrials] S1 [segment 300-500]\n");
                        printTimingsCsv(driver.stageTimings());
                        driver.resetStageTimings();
                    }
                }
            }

            // Phase: report. Timing output never feeds sim state.
            // NOTE: fprintf — println trips consteval _Format_checker C3546 here.
            std::fprintf(stderr, "[timetrials] S1 steady seed=%llu agents=%u ticks=%u\n",
                static_cast<unsigned long long>(kTrialSeed), static_cast<unsigned>(kTrialAgents),
                static_cast<unsigned>(kTrialTicks));
            if (not first_error) {
                std::fprintf(stderr, "[timetrials] S1 [segment 500-600]\n");
                printTimingsCsv(driver.stageTimings());
            }

            if (const std::error_code error = driver.shutdown()) {
                if (not first_error) {
                    first_error = error;
                }
            }
            if (first_error) {
                return std::unexpected{first_error};
            }
            return {};
        }

        [[nodiscard]] Expected<void> runWallBuilding() {
            ColonyDriver driver{};
            if (const std::error_code error =
                    driver.init(ColonyDriverDesc{.m_seed = kTrialSeed, .m_agent_count = kTrialAgents})) {
                return std::unexpected{error};
            }
            driver.setPaused(true);
            driver.resetStageTimings();

            // Phase: step. Alternating build/demolish schedule on top of the
            // steady load; budgets and step order are untouched (the errand
            // backlog is the measurement point). presentEdits is skipped:
            // headless, no translator. Per-window issued cells are the
            // probe-counted write sets issued in that window; dirty chunks
            // are the grid-dirty count at the window edge.
            // issued_cells: probe-counted at enqueue (issued, not completed).
            // dirty_chunks: dirty-chunk snapshot, not collider-processed count.
            constexpr u64 kWallCells = static_cast<u64>(kWallWidth) * static_cast<u64>(kWallHeight);
            std::error_code first_error{};
            u64 window_issued = 0u;
            driver.grid().clearAllDirty();
            for (u32 tick = 0u; tick < kTrialTicks and not first_error; ++tick) {
                if (tick % kWallPeriod == 0u) {
                    const u32 period = tick / kWallPeriod;
                    const sim::GlobalCellPos origin = kWallRects[(period / 2u) % 4u];
                    u32 rock = 0u;
                    for (u32 dy = 0u; dy < kWallHeight and not first_error; ++dy) {
                        for (u32 dx = 0u; dx < kWallWidth; ++dx) {
                            const auto cell = driver.grid().getCell({origin.m_x + dx, origin.m_y + dy});
                            if (not cell) {
                                first_error = cell.error();
                                break;
                            }
                            if (cell->m_element == kElementRock) {
                                ++rock;
                            }
                        }
                    }
                    if (not first_error) {
                        const bool base_build = period % 2u == 0u;
                        u64 writes = base_build ? kWallCells - static_cast<u64>(rock) : static_cast<u64>(rock);
                        bool build = base_build;
                        if (writes == 0u) {
                            build = not base_build;
                            writes = kWallCells;
                        }
                        if (build) {
                            const Expected<sim::Entity> wall = driver.buildWall(Footprint{.m_min = origin,
                                .m_width = kWallWidth,
                                .m_height = kWallHeight,
                                .m_element = kElementRock});
                            if (not wall) {
                                first_error = wall.error();
                                break;
                            }
                        } else {
                            const Expected<sim::Entity> teardown =
                                driver.demolish(origin, kWallWidth, kWallHeight);
                            if (not teardown) {
                                first_error = teardown.error();
                                break;
                            }
                        }
                        window_issued += writes;
                    }
                }
                if (not first_error) {
                    first_error = driver.stepOne();
                }
                if (not first_error) {
                    if (tick + 1u == 300u) {
                        const u32 dirty = driver.grid().dirtyChunkCount();
                        std::fprintf(stderr, "[timetrials] S2 [segment 0-300] issued_cells=%llu dirty_chunks=%u\n",
                            static_cast<unsigned long long>(window_issued), static_cast<unsigned>(dirty));
                        printTimingsCsv(driver.stageTimings());
                        driver.resetStageTimings();
                        driver.grid().clearAllDirty();
                        window_issued = 0u;
                    } else if (tick + 1u == 500u) {
                        const u32 dirty = driver.grid().dirtyChunkCount();
                        std::fprintf(stderr,
                            "[timetrials] S2 [segment 300-500] issued_cells=%llu dirty_chunks=%u\n",
                            static_cast<unsigned long long>(window_issued), static_cast<unsigned>(dirty));
                        printTimingsCsv(driver.stageTimings());
                        driver.resetStageTimings();
                        driver.grid().clearAllDirty();
                        window_issued = 0u;
                    }
                }
            }

            // Phase: report. Timing output never feeds sim state.
            // NOTE: fprintf — println trips consteval _Format_checker C3546 here.
            std::fprintf(stderr,
                "[timetrials] S2 wall-building seed=%llu agents=%u ticks=%u period=%u (presentEdits skipped: no translator headless)\n",
                static_cast<unsigned long long>(kTrialSeed), static_cast<unsigned>(kTrialAgents),
                static_cast<unsigned>(kTrialTicks), static_cast<unsigned>(kWallPeriod));
            if (not first_error) {
                const u32 dirty = driver.grid().dirtyChunkCount();
                std::fprintf(stderr, "[timetrials] S2 [segment 500-600] issued_cells=%llu dirty_chunks=%u\n",
                    static_cast<unsigned long long>(window_issued), static_cast<unsigned>(dirty));
                printTimingsCsv(driver.stageTimings());
            }

            if (const std::error_code error = driver.shutdown()) {
                if (not first_error) {
                    first_error = error;
                }
            }
            if (first_error) {
                return std::unexpected{first_error};
            }
            return {};
        }
    } // namespace

    Expected<void> runTimeTrials() {
        if (Expected<void> steady = runSteady(); not steady) {
            return steady;
        }
        return runWallBuilding();
    }
} // namespace pP::colony
