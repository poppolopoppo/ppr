module;

export module engine.sim:tick;

import :ecs;

import engine.core;

import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // fixed timestep
    // ------------------------------------------------------------------

    /// What to do with whole slices that exceed the per-advance slice budget:
    /// `clamp` discards the excess (the clock falls behind and catches up),
    /// `defer` keeps it as backlog for later advances.
    enum class EOverrunPolicy : u8 {
        clamp,
        defer,
    };

    struct TickReport {
        /// Whole slices consumed by this advance, never above the slice budget.
        u32 m_slices{};

        /// Unsliced time still held after this advance; grows under `defer`.
        TimeSpan m_backlog{};

        /// True when the advance produced more whole slices than the budget.
        bool m_overran{};

        [[nodiscard]] constexpr bool operator==(const TickReport &) const noexcept = default;
    };

    /// Invoked once per overrunning advance with the report just produced.
    using OverrunHook = std23::move_only_function<void(const TickReport &)>;

    /// Owning callback invoked with the caller's simulation registry.
    using SystemFn = std23::move_only_function<void(Registry &)>;

    /// Fixed-timestep accumulator: converts variable elapsed time into a bounded
    /// number of whole slices plus an interpolation `alpha()`.
    class FixedTimestep {
    private:
        TimeSpan m_period;
        TimeSpan m_backlog{};
        u32 m_max_slices{};
        EOverrunPolicy m_policy{EOverrunPolicy::clamp};
        OverrunHook m_overrun_hook{};

    public:
        /// `period` is the slice length and must be positive; `maxSlices` is the
        /// per-advance budget and must be non-zero.
        FixedTimestep(TimeSpan period, u32 maxSlices, EOverrunPolicy policy = EOverrunPolicy::clamp);

        [[nodiscard]] constexpr TimeSpan period() const noexcept {
            return m_period;
        }

        [[nodiscard]] constexpr u32 maxSlices() const noexcept {
            return m_max_slices;
        }

        [[nodiscard]] constexpr EOverrunPolicy overrunPolicy() const noexcept {
            return m_policy;
        }

        /// Fraction of one period already accumulated, clamped to [0, 1].
        [[nodiscard]] double alpha() const noexcept;

        void reset() noexcept;

        /// Replaces the overrun hook; an empty hook disables notification.
        void setOverrunHook(OverrunHook hook);

        /// Feeds `elapsed` into the accumulator and consumes up to `maxSlices`
        /// whole slices. Returns `invalid_argument` for negative elapsed time.
        [[nodiscard]] Expected<TickReport> advance(TimeSpan elapsed);
    };

    // ------------------------------------------------------------------
    // StepRegistry — declared system order without back-edges
    // ------------------------------------------------------------------

    /// Ordered registry of named simulation steps. A declaration may name steps
    /// that do not exist yet: forward references create pending placeholders that
    /// are ordered like any other step but are not scheduled until declared. Every
    /// declaration is validated before it is committed, so an empty or duplicate
    /// name is rejected with `std::errc::invalid_argument` and a self-reference or
    /// a cycle (back-edge) with `std::errc::resource_deadlock_would_occur`, leaving the registry
    /// exactly as it was.
    class StepRegistry {
    private:
        struct Step {
            std::string m_name{};
            Array<u32> m_after{};
            Array<u32> m_before{};
            bool m_declared{false};
        };

        Array<Step> m_steps;
        Array<u32> m_order;
        Array<SystemFn> m_systems;

    public:
        /// Declares `name` with no ordering constraints, or completes a pending
        /// placeholder created by an earlier forward reference.
        [[nodiscard]] Expected<u32> declare(std::string_view name);

        /// Declares `name` ordered after every entry of `after`.
        [[nodiscard]] Expected<u32> declare(std::string_view name, std::span<const std::string_view> after);

        /// Attaches one nonempty system to an already declared step. Unknown ids,
        /// pending placeholders, empty functions and repeated binds fail closed.
        [[nodiscard]] Expected<void> addSystem(u32 step, SystemFn fn);

        /// Runs all declared systems once in order(). Fails before invoking any
        /// system if even one declared step is unbound. Structural changes to the
        /// registry are allowed between systems, not during View iteration.
        [[nodiscard]] std::error_code runSystems(Registry &registry);

        /// Number of entries, including pending placeholders.
        [[nodiscard]] u32 size() const noexcept;

        [[nodiscard]] std::optional<u32> find(std::string_view name) const noexcept;

        [[nodiscard]] std::string_view name(u32 step) const noexcept;

        /// Declared steps in a stable topological order: ties break by declaration
        /// id and pending placeholders are skipped until they are declared.
        [[nodiscard]] std::span<const u32> order() const noexcept;

    private:
        [[nodiscard]] static std::optional<u32> indexOf(const Array<Step> &steps, std::string_view name) noexcept;

        /// Topologically orders `steps` into `out` using the reverse edges of
        /// `Step::m_before`; returns false on a cycle (leaving `out` unspecified).
        [[nodiscard]] static bool resolve(const Array<Step> &steps, Array<u32> &out);
    };
}
