export module game.colony;

import engine.core;
import engine.sim;
import game.colony.worldgen;

import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // colony lifecycle
    // ------------------------------------------------------------------

    /// Legacy fixture input. The seed is the only degree of freedom:
    /// grid dimensions are fixed by `engine.sim` (`kWorldEdge` = 4096).
    struct ColonyDesc {
        u64 m_seed{};

        [[nodiscard]] constexpr bool operator==(const ColonyDesc &) const noexcept = default;
    };

    /// RAII owner of authored sessions or an explicitly requested legacy fixture.
    /// install adopts authored output; shutdown releases either owner.
    class Colony {
    private:
        sim::ChunkGrid m_grid{}; // Explicit legacy fixtures only when no prepared world is installed.
        std::unique_ptr<PreparedWorld> m_prepared{};
        bool m_initialized{false};

    public:
        Colony();

        Colony(const Colony &) = delete;

        Colony &operator=(const Colony &) = delete;

        Colony(Colony &&) noexcept;

        Colony &operator=(Colony &&) noexcept;

        ~Colony();

        [[nodiscard]] const sim::ChunkGrid &grid() const noexcept;

        /// Mutable grid for explicit edit functions (`buildWall`/`demolish`
        /// on the driver). All edits must flow through those functions so
        /// presentation and search invalidation stay coherent.
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;

        [[nodiscard]] bool isInitialized() const noexcept;

        [[nodiscard]] std::error_code initLegacyFixture(
            const ColonyDesc &desc, const sim::GenerationExecution *execution = nullptr);

        /// Adopts a validated candidate without rerunning generation.
        [[nodiscard]] std::error_code install(PreparedWorld world);
        [[nodiscard]] const PreparedWorld *preparedWorld() const noexcept;

        [[nodiscard]] std::error_code shutdown();
    };
}
