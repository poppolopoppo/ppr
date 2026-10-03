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
}
