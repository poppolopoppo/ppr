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
}
