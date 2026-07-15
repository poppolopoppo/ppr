module;
#include <utility>

#include "pP/Macros.h"
export module engine.app:window.monitor;

import :service.window;
import engine.core;
import engine.math;
import std;

export namespace pP {
    struct VideoMode {
        int2 m_resolution{};
        int3 m_rgb_bits{};
        int m_refresh_rate{};

        bool operator==(const VideoMode &) const;
    };

    class Monitor : public safe_object {
    public:
        MonitorHandle m_handle{};

        std::string m_name{};

        VideoMode m_video_mode{};

        int2 m_virtual_position{};
        int2 m_physical_size{};

        bool m_primary_monitor{};

        Monitor(MonitorHandle handle,
                std::string name,
                VideoMode mode,
                const int2 &physical_size,
                const int2 &virtual_position,
                bool primary_monitor) noexcept;

        Monitor(const Monitor &) = delete;

        Monitor &operator=(const Monitor &) = delete;

        Monitor(Monitor &&other) noexcept;

        Monitor &operator=(Monitor &&other) noexcept = delete;

        [[nodiscard]] MonitorHandle release() noexcept;
    };
}
