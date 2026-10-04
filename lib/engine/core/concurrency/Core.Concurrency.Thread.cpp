module;
#include "pP/Macros.h"

module engine.core;
import :concurrency.thread;
import std;

namespace pP {
    // ------------------------------------------------------------------
    // Thread meta infos & group to add assertions
    // ------------------------------------------------------------------

    static thread_local thread::GroupId g_thread_group_id_tls{none_v};

    bool thread::Group::operator()() const noexcept {
        return this_thread::getGroup().m_id == m_id;
    }

    thread::Group this_thread::getGroup() noexcept {
        return thread::Group{g_thread_group_id_tls};
    }

    void this_thread::setGroup(const thread::Group group) noexcept {
        g_thread_group_id_tls = group.m_id;
    }

    // ------------------------------------------------------------------
    // go-like's WaitGroup to synchronize multiple tasks
    // ------------------------------------------------------------------

    WaitGroup::WaitGroup(const size_type initial_count) : m_state(std::make_shared<aligned_state_type>(initial_count)) {
    }

    void WaitGroup::add(const size_type n) const noexcept {
        const std::size_t prev = m_state->m_count.fetch_add(n);
        PPR_ASSERT(prev <= std::numeric_limits<size_type>::max() - n);
    }

    bool WaitGroup::done(const std::error_code err) const noexcept {
        if (err) [[unlikely]] {
            const std::unique_lock done_guard{m_state->m_mutex};
            if (not m_state->m_error) {
                m_state->m_error = err;
            }
        }

        const size_type prev = m_state->m_count.fetch_sub(1u);
        PPR_ASSERT(prev > 0u && "WaitGroup::done() called too many times");

        if (prev == 1u) {
            const std::unique_lock done_guard{m_state->m_mutex};
            m_state->m_done.notify_all();
        }
        return prev == 1u;
    }

    std::error_code WaitGroup::error() const noexcept {
        std::unique_lock done_guard{m_state->m_mutex};
        return m_state->m_error;
    }

    std::error_code WaitGroup::wait() const noexcept {
        std::unique_lock done_guard{m_state->m_mutex};
        m_state->m_done.wait(done_guard, [this] { return m_state->m_count == 0u; });
        return std::exchange(m_state->m_error, std::error_code{});
    }
}
