module;
#include "pP/Macros.h"
export module engine.core:concurrency.thread;

import :assert;
import :hal;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // Thread meta infos & group to add assertions
    // ------------------------------------------------------------------

    namespace thread {
        using GroupId = Numeric<u64, std::thread>;

        struct Group final {
            GroupId m_id;

            explicit constexpr Group(const GroupId id) : m_id(id) {
            }

            // ReSharper disable once CppNonExplicitConversionOperator
            [[nodiscard]] operator GroupId() const noexcept {
                return m_id;
            }

            [[nodiscard]] bool operator()() const noexcept;
        };


        constexpr Group async_group{GroupId("_Async__")};
        constexpr Group io_group{GroupId("__InOut_")};
        constexpr Group main_group{GroupId("__Main__")};
        constexpr Group worker_group{GroupId("_Worker_")};
    }

    namespace this_thread {
        [[nodiscard]] thread::Group getGroup() noexcept;
        void setGroup(thread::Group group) noexcept;
    }

    // ------------------------------------------------------------------
    // go-like's WaitGroup to synchronize multiple tasks
    // ------------------------------------------------------------------

    class [[nodiscard]] WaitGroup final {
    public:
        using size_type = std::size_t;

        explicit WaitGroup(size_type initial_count = 0u);

        void add(size_type n = 1u) const noexcept;

        bool done(std::error_code err = {}) const noexcept; // NOLINT(*-use-nodiscard)

        [[nodiscard]] std::error_code wait() const noexcept;

    private:
        struct SharedState_ {
            std::atomic<size_type> m_count{0u};
            std::error_code m_error{};
            std::condition_variable m_done;
            std::mutex m_mutex;

            explicit SharedState_(const std::size_t initial_count) noexcept : m_count(initial_count) {
            }

#if PPR_ENABLE_ASSERTIONS
            ~SharedState_() {
                PPR_ASSERT(m_count == 0u && "missing at least call to WaitGroup::done()");
            }
#endif
        };

        using aligned_state_type = alignas_padded<SharedState_, std::align_val_t{hal::cacheline_size_v}>;

        std::shared_ptr<aligned_state_type> m_state;
    };
}
