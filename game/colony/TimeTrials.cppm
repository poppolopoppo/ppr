export module game.colony.timetrials;

import engine.core;
import std;

export namespace pP::colony {
    /// Deterministic headless in-app timing runner (trials only, no renderer).
    /// S1 steps a steady world, S2 steps a scripted wall-building world, and
    /// each prints one driver `stage,calls,total_us,mean_us` CSV to stdout.
    [[nodiscard]] Expected<void> runTimeTrials();
}
