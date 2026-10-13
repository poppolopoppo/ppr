module;
#include <utility>

#include "pP/Macros.h"
export module engine.app:window.monitor;

import :service.window;
import engine.core;
import engine.math;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // video mode
    // ------------------------------------------------------------------

    struct VideoMode {
        int2 m_resolution{zero_v};
        int3 m_rgb_bits{zero_v};
        int m_refresh_rate{};

        bool operator==(const VideoMode &) const;
    };

    // ------------------------------------------------------------------
    // monitor
    // ------------------------------------------------------------------

    class Monitor : public safe_object {
    public:
        MonitorHandle m_handle{};

        std::string m_name{};
        /// OS monitor interface identity; empty when this backend cannot provide one.
        std::string m_identity{};

        VideoMode m_video_mode{};

        int2 m_virtual_position{zero_v};
        int2 m_physical_size{zero_v};

        bool m_primary_monitor{};

        Monitor(
            MonitorHandle handle, std::string name, VideoMode mode, const int2 &physical_size,
            const int2 &virtual_position, bool primary_monitor, std::string identity = {}) noexcept;

        Monitor(const Monitor &) = delete;

        Monitor &operator=(const Monitor &) = delete;

        Monitor(Monitor &&other) noexcept;

        Monitor &operator=(Monitor &&other) noexcept = delete;

        [[nodiscard]] MonitorHandle release() noexcept;
    };
}
