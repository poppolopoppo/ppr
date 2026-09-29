export module game.colony;

import engine.core;
import engine.sim;

import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // colony lifecycle
    // ------------------------------------------------------------------

    /// Typed world-generation input. The seed is the only degree of freedom:
    /// grid dimensions are fixed by `engine.sim` (`kWorldEdge` = 4096).
    struct ColonyDesc {
        u64 m_seed{};

        [[nodiscard]] constexpr bool operator==(const ColonyDesc &) const noexcept = default;
    };

    /// RAII owner of the colony simulation grid. `init` runs the offline
    /// deterministic generator into the owned grid; `shutdown` releases it.
    /// Re-init regenerates from the new descriptor; `shutdown` is idempotent.
    class Colony {
    private:
        sim::ChunkGrid m_grid{};
        bool m_initialized{false};

    public:
        Colony() = default;

        Colony(const Colony &) = delete;

        Colony &operator=(const Colony &) = delete;

        Colony(Colony &&) noexcept = default;

        Colony &operator=(Colony &&) noexcept = default;

        ~Colony() = default;

        [[nodiscard]] const sim::ChunkGrid &grid() const noexcept;

        /// Mutable grid for explicit edit functions (`buildWall`/`demolish`
        /// on the driver). All edits must flow through those functions so
        /// presentation and search invalidation stay coherent.
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;

        [[nodiscard]] bool isInitialized() const noexcept;

        [[nodiscard]] std::error_code init(const ColonyDesc &desc);

        [[nodiscard]] std::error_code shutdown();
    };
}
