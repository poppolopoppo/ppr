module;
#include "pP/Macros.h"
export module engine.core:command_history;
import :containers.stl;
import :function.ref;
import :types;
import :logger;
import std;

export namespace pP {
    PPR_DECLARE_LOG_CATEGORY(CommandHistory)
    // Owning, content-free history. Operations are borrowed only for this call.
    // A failed client operation must leave client state unchanged. Command moves
    // must not throw, so a successful apply always obtains an undo entry.
    template<typename Command>
        requires (std::is_nothrow_move_constructible_v<Command> and std::is_nothrow_move_assignable_v<Command>)
    class CommandHistory {
        struct Entry {
            Command m_command;
            u64 m_payload_bytes{};
        };
        Array<Entry> m_entries;
        std::size_t m_cursor{};
        std::size_t m_max_commands;
        u64 m_max_payload_bytes;
        u64 m_payload_bytes{};

    public:
        explicit CommandHistory(
            const std::size_t max_commands = 128u, const u64 max_payload_bytes = 8u * 1024u * 1024u) noexcept
            : m_max_commands(max_commands), m_max_payload_bytes(max_payload_bytes) {
        }
        [[nodiscard]] bool canUndo() const noexcept {
            return m_cursor != 0u;
        }
        [[nodiscard]] bool canRedo() const noexcept {
            return m_cursor != m_entries.size();
        }
        [[nodiscard]] u64 payloadBytes() const noexcept {
            return m_payload_bytes;
        }
        void clear() noexcept {
            m_entries.clear();
            m_cursor = 0u;
            m_payload_bytes = 0u;
        }
        [[nodiscard]] std::error_code execute(
            Command command, const u64 payload_bytes,
            const std23::function_ref<std::error_code(const Command &)> apply) {
            if (m_max_commands == 0u or payload_bytes > m_max_payload_bytes) {
                PPR_RETURN_ERROR_ON_FAIL(CommandHistory, std::errc::no_buffer_space);
            }
            m_entries.reserve(std::min(m_entries.size() + 1u, m_max_commands));
            PPR_RETURN_ERROR_ON_FAIL(CommandHistory, apply(command));
            while (m_entries.size() > m_cursor) {
                m_payload_bytes -= m_entries.back().m_payload_bytes;
                m_entries.pop_back();
            }
            while (m_entries.size() >= m_max_commands or payload_bytes > m_max_payload_bytes - m_payload_bytes) {
                m_payload_bytes -= m_entries.front().m_payload_bytes;
                m_entries.erase(m_entries.begin());
            }
            m_entries.push_back({std::move(command), payload_bytes});
            m_payload_bytes += payload_bytes;
            m_cursor = m_entries.size();
            return {};
        }
        // false is expected absence; unexpected means the client operation failed.
        [[nodiscard]] std::expected<bool, std::error_code> undo(
            const std23::function_ref<std::error_code(const Command &)> revert) {
            if (not canUndo()) {
                return false;
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(CommandHistory, revert(m_entries[m_cursor - 1u].m_command));
            --m_cursor;
            return true;
        }
        [[nodiscard]] std::expected<bool, std::error_code> redo(
            const std23::function_ref<std::error_code(const Command &)> apply) {
            if (not canRedo()) {
                return false;
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(CommandHistory, apply(m_entries[m_cursor].m_command));
            ++m_cursor;
            return true;
        }
    };
}
