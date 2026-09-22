module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP::hal::timer {

struct TimerData {
    std::function<void()> m_callback;
    std::atomic<bool> m_fired{false};
    std::jthread m_thread{};
};

DeadlineHandle setDeadline(std::chrono::milliseconds ms, std::function<void()> callback) noexcept(false) {
    auto *data = new TimerData{std::move(callback)};
    data->m_thread = std::jthread([data, ms](std::stop_token st) noexcept {
        std::this_thread::sleep_for(ms);
        if (st.stop_requested()) {
            delete data;
            return;
        }
        if (!data->m_fired.exchange(true)) {
            data->m_callback();
        }
        delete data;
    });
    return DeadlineHandle{data};
}

void cancelDeadline(DeadlineHandle &handle) noexcept {
    if (handle.m_data) {
        auto *data = static_cast<TimerData*>(handle.m_data);
        data->m_fired = true;
        data->m_thread.request_stop();
        if (data->m_thread.joinable()) {
            data->m_thread.join();
        }
        handle.m_data = nullptr;
    }
}

}
