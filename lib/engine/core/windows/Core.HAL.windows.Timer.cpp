module;

#include "Core.HAL.windows.include.h"

module engine.core;

import :hal;

import std;

namespace pP::hal::timer {

struct TimerData {
    std::move_only_function<void()> m_callback;
    std::atomic<bool> m_fired{false};
    HANDLE h_timer{nullptr};
};

DeadlineHandle setDeadline(std::chrono::milliseconds ms, std::move_only_function<void()> callback) noexcept(false) {
    auto *data = new TimerData{std::move(callback)};

    HANDLE hTimer = nullptr;
    if (!::CreateTimerQueueTimer(
        &hTimer, nullptr,
        [](void *param, BOOLEAN) noexcept {
            auto *data = static_cast<TimerData*>(param);
            if (!data->m_fired.exchange(true)) {
                data->m_callback();
            }
            delete data;
        },
        data, static_cast<DWORD>(ms.count()), 0, WT_EXECUTEDEFAULT)) {
        delete data;
        throw std::runtime_error("CreateTimerQueueTimer failed");
    }

    data->h_timer = hTimer;
    return DeadlineHandle{data};
}

void cancelDeadline(DeadlineHandle &handle) noexcept {
    if (handle.m_data) {
        auto *data = static_cast<TimerData*>(handle.m_data);
        data->m_fired = true;
        ::DeleteTimerQueueTimer(nullptr, data->h_timer, INVALID_HANDLE_VALUE);
        handle.m_data = nullptr;
    }
}

}
