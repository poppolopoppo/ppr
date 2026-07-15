module;

#include "pP/Macros.h"

module engine.app;

import :window.monitor;
import engine.core;
import std;

namespace pP {
    bool VideoMode::operator==(const VideoMode &other) const = default;

    Monitor::Monitor(const MonitorHandle handle,
                     std::string name,
                     VideoMode mode,
                     const int2 &physical_size,
                     const int2 &virtual_position,
                     const bool primary_monitor) noexcept
        : m_handle(handle),
          m_name(std::move(name)),
          m_video_mode(std::move(mode)),
          m_virtual_position(virtual_position),
          m_physical_size(physical_size),
          m_primary_monitor(primary_monitor) {
    }

    Monitor::Monitor(Monitor &&other) noexcept
        : m_handle(other.m_handle),
          m_name(std::move(other.m_name)),
          m_video_mode(other.m_video_mode),
          m_virtual_position(other.m_virtual_position),
          m_physical_size(other.m_physical_size) {
        other.m_handle = zero_v;
    }

    MonitorHandle Monitor::release() noexcept {
        PPR_ASSERT(m_handle != nullptr);
        MonitorHandle release{nullptr};
        swap(release, m_handle);
        return release;
    }
}
