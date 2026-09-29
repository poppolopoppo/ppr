module;
#include "pP/Macros.h"

module engine.sim;

import :ecs;

import engine.core;

import std;

namespace pP::sim {
    // ------------------------------------------------------------------
    // ecs error category
    // ------------------------------------------------------------------

    namespace {
        class EcsErrorCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "sim.ecs";
            }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<ecs::errc>(ev)) {
                    case ecs::errc::ok: return "indicates success";
                    case ecs::errc::too_many_components:
                        return "component capacity exceeded (kMaxComponents)";
                    default: return "unknown sim.ecs result (" + std::to_string(ev) + ")";
                }
            }
        };
    }

    namespace ecs {
        const std::error_category &error_category() noexcept {
            static const EcsErrorCategory g_ecs_error_category{};
            return g_ecs_error_category;
        }

        std::error_code make_error_code(const errc err) noexcept {
            return std::error_code{static_cast<int>(err), error_category()};
        }
    }

    // ------------------------------------------------------------------
    // Registry — slots
    // ------------------------------------------------------------------

    Entity Registry::create() {
        u32 index = 0u;

        if (m_free_head != none_v) {
            // Recycled slot: it carries the generation `destroy` bumped it to.
            index = m_free_head;
            m_free_head = m_slots[index].m_free_next;
            m_slots[index].m_free_next = none_v;
        } else {
            // Fresh slot: generations start at 1, 0 stays reserved as invalid.
            index = safe_narrowing(m_slots.size());
            m_slots.emplace_back().m_generation = 1u;
        }

        Slot &slot = m_slots[index];
        slot.m_alive = true;
        slot.m_mask = ComponentMask{};
        ++m_alive_count;

        return Entity{index, slot.m_generation};
    }

    void Registry::destroy(const Entity entity) {
        PPR_ASSERT(isAlive(entity) and "destroy targets a live entity");

        const u32 index = entity.m_index;
        Slot &slot = m_slots[index];

        // Every held column loses its row first: the tail swap keeps rows dense
        // and the moved entity's reverse lookup follows the tail.
        const u32 columns = componentCount();
        for (u32 id = 0u; id < columns; ++id) {
            if (slot.m_mask.test(id)) {
                swapRemoveRow(m_columns[id], m_columns[id].m_rows[index]);
            }
        }

        slot.m_mask = ComponentMask{};
        slot.m_alive = false;

        u32 generation = slot.m_generation + 1u;
        if (generation == 0u) [[unlikely]] {
            generation = 1u; // skip 0: it is reserved as the invalid marker
        }
        slot.m_generation = generation;

        slot.m_free_next = m_free_head;
        m_free_head = index;
        --m_alive_count;
    }

    void Registry::clear() noexcept {
        m_slots.clear();
        for (Column &column: m_columns) {
            column.m_values.clear();
            column.m_owners.clear();
            column.m_rows.clear();
        }
        m_alive_count = 0u;
        m_free_head = none_v;
    }

    EcsSection Registry::dump() const {
        EcsSection section{};
        section.m_generations.reserve(m_slots.size());
        section.m_alive.reserve(m_slots.size());
        for (const Slot &slot: m_slots) {
            section.m_generations.push_back(slot.m_generation);
            section.m_alive.push_back(slot.m_alive ? 1u : 0u);
        }
        for (u32 index = m_free_head; index != none_v; index = m_slots[index].m_free_next) {
            section.m_free_order.push_back(index);
        }

        section.m_columns.reserve(m_columns.size());
        for (ComponentId id = 0u; id < m_columns.size(); ++id) {
            const Column &column = m_columns[id];
            EcsColumnData data{};
            data.m_id = id;
            data.m_stride = column.m_stride;
            data.m_owners = column.m_owners;
            data.m_bytes.reserve(column.m_values.size());
            for (const std::byte value: column.m_values) {
                data.m_bytes.push_back(std::to_integer<u8>(value));
            }
            section.m_columns.push_back(std::move(data));
        }
        return section;
    }

    std::error_code Registry::restore(const EcsSection &section) {
        const std::error_code invalid = std::make_error_code(std::errc::invalid_argument);
        if (section.m_ecs_version != 1u or
            section.m_generations.size() != section.m_alive.size() or
            section.m_generations.size() > std::numeric_limits<u32>::max() or
            section.m_columns.size() != m_columns.size()) {
            return invalid;
        }

        for (std::size_t index = 0u; index < section.m_alive.size(); ++index) {
            if (section.m_generations[index] == 0u or section.m_alive[index] > 1u) {
                return invalid;
            }
        }

        Array<u8> free_seen(section.m_generations.size(), 0u);
        std::size_t dead_count = 0u;
        for (const u8 alive: section.m_alive) {
            dead_count += alive == 0u;
        }
        if (section.m_free_order.size() != dead_count) {
            return invalid;
        }
        for (const u32 index: section.m_free_order) {
            if (index >= section.m_generations.size() or section.m_alive[index] != 0u or free_seen[index] != 0u) {
                return invalid;
            }
            free_seen[index] = 1u;
        }

        for (ComponentId id = 0u; id < m_columns.size(); ++id) {
            const EcsColumnData &data = section.m_columns[id];
            if (data.m_id != id or
                data.m_stride != m_columns[id].m_stride or
                data.m_stride == 0u or
                data.m_bytes.size() % data.m_stride != 0u or
                data.m_bytes.size() / data.m_stride != data.m_owners.size()) {
                return invalid;
            }

            Array<u8> seen(section.m_generations.size(), 0u);
            for (const Entity owner: data.m_owners) {
                if (owner.m_index >= section.m_generations.size() or
                    section.m_alive[owner.m_index] != 1u or
                    section.m_generations[owner.m_index] != owner.m_generation or
                    seen[owner.m_index] != 0u) {
                    return invalid;
                }
                seen[owner.m_index] = 1u;
            }
        }

        clear();
        m_slots.resize(section.m_generations.size());
        for (u32 index = 0u; index < m_slots.size(); ++index) {
            Slot &slot = m_slots[index];
            slot.m_generation = section.m_generations[index];
            slot.m_alive = section.m_alive[index] == 1u;
            if (slot.m_alive) {
                ++m_alive_count;
            }
        }

        for (auto it = section.m_free_order.rbegin(); it != section.m_free_order.rend(); ++it) {
            m_slots[*it].m_free_next = m_free_head;
            m_free_head = *it;
        }

        for (const EcsColumnData &data: section.m_columns) {
            for (std::size_t row = 0u; row < data.m_owners.size(); ++row) {
                appendRow(data.m_id, data.m_owners[row],
                    reinterpret_cast<const std::byte *>(data.m_bytes.data() + row * data.m_stride));
            }
        }
        return {};
    }

    bool Registry::isAlive(const Entity entity) const noexcept {
        if (entity.m_index >= m_slots.size()) [[unlikely]] {
            return false;
        }

        const Slot &slot = m_slots[entity.m_index];
        return entity.isValid() and slot.m_alive and slot.m_generation == entity.m_generation;
    }

    ComponentMask Registry::mask(const Entity entity) const noexcept {
        if (not isAlive(entity)) {
            return ComponentMask{};
        }

        return m_slots[entity.m_index].m_mask;
    }

    u32 Registry::aliveCount() const noexcept {
        return m_alive_count;
    }

    u32 Registry::slotCount() const noexcept {
        return safe_narrowing(m_slots.size());
    }

    u32 Registry::componentCount() const noexcept {
        return safe_narrowing(m_columns.size());
    }

    // ------------------------------------------------------------------
    // Registry — columns
    // ------------------------------------------------------------------

    Expected<ComponentId> Registry::registerColumn(const std::type_index type, const u32 stride) {
        if (m_columns.size() >= kMaxComponents) [[unlikely]] {
            return std::unexpected{ecs::make_error_code(ecs::errc::too_many_components)};
        }

        const ComponentId id = safe_narrowing(m_columns.size());

        Column column{};
        column.m_type = type;
        column.m_stride = stride;
        m_columns.push_back(std::move(column));

        return id;
    }

    void Registry::appendRow(const ComponentId id, const Entity entity, const std::byte *src) {
        Column &column = m_columns[id];

        // The reverse lookup covers every slot that exists once it holds a row;
        // slots created later only materialize it when they first get a row.
        if (column.m_rows.size() < m_slots.size()) {
            column.m_rows.resize(m_slots.size(), none_v);
        }

        const u32 row = safe_narrowing(column.m_owners.size());
        column.m_values.insert(column.m_values.end(), src, src + column.m_stride);
        column.m_owners.push_back(entity);
        column.m_rows[entity.m_index] = row;

        m_slots[entity.m_index].m_mask.set(id);
    }

    void Registry::swapRemoveRow(Column &column, const u32 index) noexcept {
        const u32 rows = safe_narrowing(column.m_owners.size());
        const u32 last_row = rows - 1u;
        PPR_ASSERT(index <= last_row and "row index stays inside the column");

        const Entity removed = column.m_owners[index];
        column.m_rows[removed.m_index] = none_v;

        if (index != last_row) {
            const Entity moved = column.m_owners[last_row];
            const std::size_t hole = static_cast<std::size_t>(index) * column.m_stride;
            const std::size_t tail = static_cast<std::size_t>(last_row) * column.m_stride;

            std::memcpy(column.m_values.data() + hole, column.m_values.data() + tail, column.m_stride);

            column.m_owners[index] = moved;
            column.m_rows[moved.m_index] = index;
        }

        column.m_values.resize(column.m_values.size() - column.m_stride);
        column.m_owners.pop_back();
    }
} // namespace pP::sim
