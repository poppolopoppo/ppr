module;
#include "pP/Macros.h"

export module engine.sim:ecs;

import engine.core;

import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // entity identity
    // ------------------------------------------------------------------

    /// Strong entity id: a slot index plus the slot's allocation generation,
    /// with the `SparseHandle` discipline (`:renderer.gpu_caches`): generations
    /// start at 1 and are bumped on every destroy (0 is skipped), so a recycled
    /// slot can never reissue an id that is still held somewhere. Handles are
    /// registry-local by contract; the default-constructed handle is invalid.
    /// Staleness is a registry relation (`Registry::isAlive`), not a property
    /// of the handle: a destroyed handle keeps a non-zero generation.
    struct Entity {
        u32 m_index = 0xFFFFFFu;
        u32 m_generation = 0u;

        constexpr Entity() noexcept = default;

        constexpr Entity(const u32 index, const u32 generation) noexcept
            : m_index(index), m_generation(generation) {
        }

        /// Generation 0 is never issued, so only the default handle is invalid.
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return m_generation != 0u;
        }

        [[nodiscard]] constexpr bool operator ==(const Entity &other) const noexcept = default;

        /// Orders by slot first, then generation — consistent with `operator==`.
        [[nodiscard]] constexpr std::strong_ordering operator <=>(const Entity &other) const noexcept {
            if (const std::strong_ordering by_index = m_index <=> other.m_index;
                by_index != std::strong_ordering::equal) {
                return by_index;
            }
            return m_generation <=> other.m_generation;
        }
    };

    static_assert(std::is_standard_layout_v<Entity>);
    static_assert(std::is_trivially_copyable_v<Entity>);
    static_assert(sizeof(Entity) == 8u);

    // ------------------------------------------------------------------
    // component vocabulary
    // ------------------------------------------------------------------

    /// Fixed per-registry component capacity: `ComponentMask` is one 64-bit
    /// word, and registration fail-closes with `ecs::errc::too_many_components`
    /// instead of growing past this bound.
    inline constexpr u32 kMaxComponents = 64u;

    /// Dense component id in [0, kMaxComponents), assigned by
    /// `Registry::registerComponent` in registration order.
    using ComponentId = u32;

    /// Bitset over registered component ids. Bit operations at
    /// `id >= kMaxComponents` are programmer invariants (`PPR_ASSERT`).
    struct ComponentMask {
        u64 m_bits{};

        constexpr void set(const ComponentId id) noexcept {
            PPR_ASSERT(id < kMaxComponents);
            m_bits |= u64{1u} << id;
        }

        constexpr void reset(const ComponentId id) noexcept {
            PPR_ASSERT(id < kMaxComponents);
            m_bits &= ~(u64{1u} << id);
        }

        [[nodiscard]] constexpr bool test(const ComponentId id) const noexcept {
            PPR_ASSERT(id < kMaxComponents);
            return ((m_bits >> id) & 1u) != 0u;
        }

        /// True when every bit of `required` is also set here (superset test).
        [[nodiscard]] constexpr bool contains(const ComponentMask required) const noexcept {
            return (m_bits & required.m_bits) == required.m_bits;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return m_bits == 0u;
        }

        [[nodiscard]] constexpr bool operator ==(const ComponentMask &other) const noexcept = default;
    };

    // ------------------------------------------------------------------
    // errors
    // ------------------------------------------------------------------

    namespace ecs {
        /// Fail-closed result of component registration. A category of its own
        /// (`sim.ecs`), shaped like `sim::errc` in `:snapshot`: `:ecs` must not
        /// import `:snapshot`.
        enum class errc : int {
            ok = 0,
            too_many_components = 1,
        };

        [[nodiscard]] const std::error_category &error_category() noexcept;

        [[nodiscard]] std::error_code make_error_code(errc err) noexcept;
    }

    /// Snapshot data contains only stable component ids and raw rows; runtime
    /// type identities are never serialized.
    struct EcsColumnData {
        ComponentId m_id{};
        u32 m_stride{};
        Array<Entity> m_owners{};
        Array<u8> m_bytes{};

        [[nodiscard]] bool operator==(const EcsColumnData &) const = default;
    };

    struct EcsSection {
        u32 m_ecs_version{1u};
        Array<u32> m_generations{};
        Array<u8> m_alive{};
        Array<u32> m_free_order{}; // free-list head to tail (next allocation first)
        Array<EcsColumnData> m_columns{};

        [[nodiscard]] bool operator==(const EcsSection &) const = default;
    };

    // ------------------------------------------------------------------
    // Registry — entity slots and SoA component columns
    // ------------------------------------------------------------------

    template<typename RegistryT, typename... Cs>
    class View;

    /// Minimal entity-component store. Entity identity lives in a slot table
    /// (`Entity::m_index` is the slot index); each registered component owns
    /// one SoA column: raw `T` bytes in an `Array<std::byte>` (row-major,
    /// `m_stride` bytes per row), the row's owning entity, and a reverse row
    /// lookup per slot. Destroying an entity swap-removes it from every column
    /// it holds — the tail row moves into the hole and the moved entity's
    /// reverse lookup follows it — so rows stay dense and lookups stay O(1).
    ///
    /// Components must be trivially copyable and not over-aligned
    /// (`alignof(T) <= alignof(std::max_align_t)`). Both gates are
    /// `static_assert`s: the columns raw-copy component bytes, so
    /// non-trivially-copyable types are refused outright with no fallback path.
    ///
    /// Single-threaded by contract: the registry takes no locks and no
    /// operation is synchronized — every access must be externally serialized
    /// on one thread. Registration can fail with `ecs::errc::too_many_components`;
    /// restore rejects malformed snapshots before changing registry state.
    /// Other lifecycle misuse is a `PPR_ASSERT` programmer invariant.
    class Registry {
    private:
        struct Slot {
            u32 m_generation = 0u; // 0 = never allocated; bumped on destroy
            u32 m_free_next = none_v; // LIFO free list; none_v terminates it
            ComponentMask m_mask{};
            bool m_alive = false;
        };

        struct Column {
            Array<std::byte> m_values{};
            Array<Entity> m_owners{}; // row -> entity
            Array<u32> m_rows{}; // entity index -> row; none_v when absent
            std::type_index m_type{typeid(void)}; // layout identity: set once at registration
            u32 m_stride = 0u;
        };

        Array<Slot> m_slots{};
        Array<Column> m_columns{}; // indexed by ComponentId (dense prefix)
        u32 m_alive_count = 0u;
        u32 m_free_head = none_v;

    public:
        /// Allocates an entity slot: a recycled slot (LIFO free list) comes
        /// back with the generation `destroy` bumped it to; a fresh slot starts
        /// at generation 1.
        [[nodiscard]] Entity create();

        /// Swap-removes `entity` from every column holding it, clears its mask,
        /// bumps its generation (0 is skipped), and recycles the slot.
        /// Precondition: `entity` is alive (`PPR_ASSERT`).
        void destroy(Entity entity);

        /// Clears all entities and component rows, retaining registration ids,
        /// types and strides. Previously issued generations are not retained.
        void clear() noexcept;

        [[nodiscard]] EcsSection dump() const;

        /// Replaces slots and rows after checking the entire section against
        /// registered column layouts. Rejection leaves the registry untouched.
        [[nodiscard]] std::error_code restore(const EcsSection &section);

        /// True only for the current generation of a live slot; stale and
        /// default handles read false and never alias a recycled slot.
        [[nodiscard]] bool isAlive(Entity entity) const noexcept;

        /// Component mask of a live entity; a stale or dead handle reads an
        /// empty mask (expected absence, not a failure).
        [[nodiscard]] ComponentMask mask(Entity entity) const noexcept;

        [[nodiscard]] u32 aliveCount() const noexcept;

        /// Entity slots, live and dead: the current `Entity::m_index` domain.
        [[nodiscard]] u32 slotCount() const noexcept;

        /// Number of registered component types.
        [[nodiscard]] u32 componentCount() const noexcept;

        /// Component id of `T` in this registry, or `nullopt` when `T` was never
        /// registered (expected absence).
        template<typename T>
        [[nodiscard]] std::optional<ComponentId> componentId() const noexcept {
            for (u32 id = 0u; id < m_columns.size(); ++id) {
                if (m_columns[id].m_type == std::type_index{typeid(T)}) {
                    return id;
                }
            }

            return std::nullopt;
        }

        /// Registers `T` as the next dense component id, or returns the id it
        /// already has (registration is idempotent). Fail-closed:
        /// `ecs::errc::too_many_components` at `kMaxComponents` types, leaving
        /// the registry unchanged. Non-trivially-copyable and over-aligned `T`
        /// are refused at compile time (see `emplace`).
        template<typename T>
        [[nodiscard]] Expected<ComponentId> registerComponent() {
            static_assert(std::is_trivially_copyable_v<T>,
                "ECS components must be trivially copyable: columns raw-copy component bytes "
                "and provide no fallback for non-trivially-copyable types");
            static_assert(alignof(T) <= alignof(std::max_align_t),
                "ECS columns are std::byte-backed with fundamental alignment only: "
                "over-aligned components are refused");

            if (const std::optional<ComponentId> existing = componentId<T>(); existing.has_value()) {
                return *existing;
            }

            return registerColumn(std::type_index{typeid(T)}, safe_narrowing(sizeof(T)));
        }

        /// Stores `value` as `entity`'s `T` row at the column tail.
        /// Precondition (`PPR_ASSERT`): `T` is registered, `entity` is alive,
        /// and the entity does not already hold `T`. The value is raw-copied
        /// into the `Array<std::byte>` column, so `T` must be trivially
        /// copyable and not over-aligned — both are compile-time refusals with
        /// no fallback path.
        template<typename T>
        void emplace(Entity entity, T value) {
            static_assert(std::is_trivially_copyable_v<T>,
                "ECS components must be trivially copyable: columns raw-copy component bytes "
                "and provide no fallback for non-trivially-copyable types");
            static_assert(alignof(T) <= alignof(std::max_align_t),
                "ECS columns are std::byte-backed with fundamental alignment only: "
                "over-aligned components are refused");

            const std::optional<ComponentId> id = componentId<T>();
            PPR_ASSERT(id.has_value() and "register T before emplacing it");
            PPR_ASSERT(isAlive(entity) and "emplace targets a live entity");
            PPR_ASSERT(not m_slots[entity.m_index].m_mask.test(*id) and "entity already holds T");

            appendRow(*id, entity, reinterpret_cast<const std::byte *>(std::addressof(value)));
        }

        /// Pointer to `entity`'s `T` component, or `nullptr` when `T` was never
        /// registered, the handle is dead or stale, or the entity does not hold
        /// `T` (expected absence, not a failure).
        template<typename T>
        [[nodiscard]] T *get(Entity entity) noexcept {
            return const_cast<T *>(std::as_const(*this).get<T>(entity));
        }

        /// Const overload: `const T *` for a const registry.
        template<typename T>
        [[nodiscard]] const T *get(Entity entity) const noexcept {
            const std::optional<ComponentId> id = componentId<T>();
            if (not id.has_value() or not isAlive(entity)) [[unlikely]] {
                return nullptr;
            }

            if (not m_slots[entity.m_index].m_mask.test(*id)) {
                return nullptr;
            }

            const Column &column = m_columns[*id];
            const u32 row = column.m_rows[entity.m_index];
            PPR_ASSERT(row != none_v and "a masked entity always has a row per held component");

            return std::launder(reinterpret_cast<const T *>(
                column.m_values.data() + static_cast<std::size_t>(row) * column.m_stride));
        }

        /// Removes `entity`'s `T` component through a single-column
        /// swap-remove (the tail row moves into the hole, the moved entity's
        /// reverse lookup follows it) and clears the mask bit. Returns true
        /// only when a row was removed; an unregistered `T`, a dead or stale
        /// handle, or an entity that does not hold `T` is expected absence and
        /// returns false without touching the registry.
        template<typename T>
        [[nodiscard]] bool remove(Entity entity) noexcept {
            const std::optional<ComponentId> id = componentId<T>();
            if (not id.has_value() or not isAlive(entity)) [[unlikely]] {
                return false;
            }

            if (not m_slots[entity.m_index].m_mask.test(*id)) {
                return false;
            }

            swapRemoveRow(m_columns[*id], m_columns[*id].m_rows[entity.m_index]);
            m_slots[entity.m_index].m_mask.reset(*id);
            return true;
        }

        /// Iterates every live entity holding all of `Cs...` in ascending slot
        /// index order: row-major over the slot table, which is creation order
        /// for a monotonically growing registry and stays the creation order of
        /// the survivors after swap-remove destroys. A requested component type
        /// that is not registered fail-closes to an empty view. Constructing
        /// and iterating a view never allocates.
        template<typename... Cs>
        [[nodiscard]] View<Registry, Cs...> view() noexcept {
            return View<Registry, Cs...>{*this, {componentId<Cs>()...}};
        }

        /// Const overload of `view()`: yields `const Cs&` references.
        template<typename... Cs>
        [[nodiscard]] View<const Registry, Cs...> view() const noexcept {
            return View<const Registry, Cs...>{*this, {componentId<Cs>()...}};
        }

    private:
        [[nodiscard]] Expected<ComponentId> registerColumn(std::type_index type, u32 stride);

        /// Appends a tail row for `entity` from `src` (`stride` bytes) and sets
        /// the entity's mask bit.
        void appendRow(ComponentId id, Entity entity, const std::byte *src);

        /// Moves `column`'s tail row into `index`'s hole, fixes the moved
        /// entity's reverse lookup, and pops both arrays.
        void swapRemoveRow(Column &column, u32 index) noexcept;

        template<typename RegistryT, typename... Cs>
        friend class View;
    };

    // ------------------------------------------------------------------
    // views
    // ------------------------------------------------------------------

    /// Zero-allocation view of a `Registry`: yields
    /// `std::tuple<Entity, Cs&...>` (const `Cs&` for a const registry) for each
    /// live entity whose mask contains every `Cs...`, in ascending slot index
    /// order. Each component is fetched O(1) through its column's reverse row
    /// lookup, so iteration never scans columns and never allocates.
    ///
    /// Precondition: no structural mutation (`create`, `destroy`, `emplace`,
    /// `remove`) while a view over the same registry is being iterated — the
    /// view holds no slot-table version, so a violated precondition is a
    /// programmer invariant, not an enforced or detected error path.
    template<typename RegistryT, typename... Cs>
    class View {
        static_assert(sizeof...(Cs) > 0u, "a view needs at least one component type");
        static_assert((std::is_trivially_copyable_v<Cs> and ...),
            "ECS components must be trivially copyable: views raw-read column bytes "
            "and provide no fallback for non-trivially-copyable types"
        );

    public:
        /// The entity plus one reference per requested component, in
        /// declaration order.
        using Reference = std::tuple<Entity, std::conditional_t<std::is_const_v<RegistryT>, const Cs, Cs> &...>;

        class Iterator {
        private:
            const View *m_view = nullptr;
            u32 m_index = 0u;

            void skipToMatch_() noexcept {
                while (m_index < m_view->slotCount_() and not m_view->matches_(m_index)) {
                    ++m_index;
                }
            }

            friend class View;

        public:
            /// The readable value: entity plus the requested components by
            /// value (the yielded reference binds them in place).
            using value_type = std::tuple<Entity, Cs...>;
            using difference_type = std::ptrdiff_t;
            using reference = Reference;
            using pointer = void;
            using iterator_category = std::forward_iterator_tag;
            using iterator_concept = std::forward_iterator_tag;

            /// Singular iterator: equality-compares only against other singular
            /// iterators; real iterators come from `View::begin`/`end`.
            Iterator() noexcept = default;

            [[nodiscard]] Reference operator*() const noexcept {
                return m_view->makeReference_(m_index, std::index_sequence_for<Cs...>{});
            }

            Iterator &operator++() noexcept {
                ++m_index;
                skipToMatch_();
                return *this;
            }

            [[nodiscard]] Iterator operator++(int) noexcept {
                const Iterator previous = *this;
                ++(*this);
                return previous;
            }

            [[nodiscard]] friend bool operator ==(const Iterator &lhs, const Iterator &rhs) noexcept {
                return lhs.m_view == rhs.m_view and lhs.m_index == rhs.m_index;
            }

        private:
            Iterator(const View &view, const u32 index) noexcept
                : m_view(std::addressof(view)), m_index(index) {
                skipToMatch_();
            }
        };

    private:
        friend class Registry;

        /// Fail-closed: any id that is `nullopt` empties the whole view.
        View(RegistryT &world, const std::array<std::optional<ComponentId>, sizeof...(Cs)> &ids) noexcept
            : m_world(std::addressof(world)) {
            for (std::size_t index = 0u; index < ids.size(); ++index) {
                if (not ids[index].has_value()) {
                    m_valid = false;
                    continue;
                }

                m_ids[index] = *ids[index];
                m_required.set(*ids[index]);
            }
        }

        RegistryT *m_world = nullptr;
        std::array<ComponentId, sizeof...(Cs)> m_ids{};
        ComponentMask m_required{};
        bool m_valid = true;

    public:
        /// First matching slot; equals `end()` when nothing matches or a
        /// requested component is unregistered.
        [[nodiscard]] Iterator begin() const noexcept {
            return Iterator{*this, m_valid ? 0u : slotCount_()};
        }

        [[nodiscard]] Iterator end() const noexcept {
            return Iterator{*this, slotCount_()};
        }

    private:
        [[nodiscard]] u32 slotCount_() const noexcept {
            return safe_narrowing(m_world->m_slots.size());
        }

        [[nodiscard]] bool matches_(const u32 index) const noexcept {
            const auto &slot = m_world->m_slots[index];
            return slot.m_alive and slot.m_mask.contains(m_required);
        }

        template<std::size_t... I>
        [[nodiscard]] Reference makeReference_(const u32 index, const std::index_sequence<I...>) const noexcept {
            const auto &slot = m_world->m_slots[index];
            return Reference{Entity{index, slot.m_generation}, componentRef_<Cs>(index, I)...};
        }

        template<typename ComponentT>
        [[nodiscard]] auto componentRef_(const u32 index, const std::size_t id_index) const noexcept
            -> std::conditional_t<std::is_const_v<RegistryT>, const ComponentT, ComponentT> & {
            auto &column = m_world->m_columns[m_ids[id_index]];
            const u32 row = column.m_rows[index];
            PPR_ASSERT(row != none_v and "a masked entity always has a row per held component");

            using ComponentRef = std::conditional_t<std::is_const_v<RegistryT>, const ComponentT, ComponentT>;
            return *std::launder(reinterpret_cast<ComponentRef *>(
                column.m_values.data() + static_cast<std::size_t>(row) * column.m_stride));
        }
    };
}

export template<>
struct std::is_error_code_enum<pP::sim::ecs::errc> : true_type { // NOLINT(*-dcl58-cpp)
};
