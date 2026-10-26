export module game.colony;

import engine.core;
import engine.sim;
import game.colony.worldgen;

import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // colony lifecycle
    // ------------------------------------------------------------------

    /// RAII owner of authored sessions; install adopts validated output.
    class Colony {
    private:
        sim::ChunkGrid m_grid{}; // Empty/uninitialized packet and component fixtures.
        std::unique_ptr<PreparedWorld> m_prepared{};
        sim::MatterParticleState m_particles{};
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

        /// Adopts a validated candidate without rerunning generation.
        [[nodiscard]] std::error_code install(
            PreparedWorld world, const sim::MatterParticleSnapshot *particles = nullptr,
            const SharedContext &context = {});
        [[nodiscard]] sim::MatterParticleState &particles() noexcept;
        [[nodiscard]] const sim::MatterParticleState &particles() const noexcept;
        [[nodiscard]] const PreparedWorld *preparedWorld() const noexcept;

        [[nodiscard]] std::error_code shutdown();
    };
}
