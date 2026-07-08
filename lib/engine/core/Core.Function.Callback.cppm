module;
#include "pP/Macros.h"
export module engine.core:function.callback;

import :assert;
import :containers.sparse_vector;
import :function.ref;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // function callback with multiple subscribers
    // ------------------------------------------------------------------

    template<typename FunctionT, mem::details::TAllocator AllocatorT = mem::GPA>
        requires std::is_function_v<FunctionT>
    class Callback {
    public:
        using Event = std23::function_ref<FunctionT>;

        SparseVectorInplace<Event, AllocatorT> m_events;

        class [[nodiscard]] Handle {
            Callback *m_callback{nullptr};
            SparseKeyId m_event_key{};

        public:
            constexpr Handle() = default;

            Handle(Callback &callback, const SparseKeyId &event_key) noexcept
                : m_callback(std::addressof(callback)), m_event_key(event_key) {
            }

            ~Handle() noexcept {
                if (m_callback != nullptr) {
                    m_callback->remove(m_event_key);
                    m_callback = nullptr;
                }
            }

            [[nodiscard]] constexpr bool isValid() const noexcept {
                return m_callback != nullptr;
            }
        };

        Callback() noexcept = default;

        explicit Callback(const AllocatorT &alloc) noexcept
            : m_events(alloc) {
        }

        explicit Callback(AllocatorT &&alloc) noexcept
            : m_events(std::forward<AllocatorT>(alloc)) {
        }

        Handle add(Event &&event) {
            if (const auto it = m_events.find(event); m_events.end() != it) [[unlikely]] {
                return Handle(*this, it.getKey());
            }
            return Handle(*this, m_events.add(std::forward<Event>(event)));
        }

        bool remove(const SparseKeyId event_key) {
            return m_events.erase(event_key);
        }

        void clear() noexcept {
            m_events.clear();
        }

        template<typename... ArgsT>
            requires requires (const Event &event, ArgsT&&... args)
        {
            event(std::forward<ArgsT>(args)...);
        }
        void trigger(ArgsT&&... args)
            noexcept(noexcept(Event{}(std::forward<ArgsT>(args)...))) {
            for (const Event &event : m_events) {
                event(std::forward<ArgsT>(args)...);
            }
        }
    };

}
