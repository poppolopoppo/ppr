module;

#include <signal.h>
#include <time.h>

module engine.core;

import :hal;

import std;

namespace pP::hal::timer {

struct TimerData {
    std::function<void()> m_callback; // move_only_function unavailable on Clang 20 + libc++ 20.1
    std::atomic<bool> m_fired{false};
    timer_t m_timer_id{};
};

DeadlineHandle setDeadline(std::chrono::milliseconds ms, std::function<void()> callback) noexcept(false) { // move_only_function unavailable on Clang 20 + libc++ 20.1
    auto *data = new TimerData{std::move(callback)};

    struct sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = data;
    sev.sigev_notify_function = [](union sigval sv) noexcept {
        auto *data = static_cast<TimerData*>(sv.sival_ptr);
        if (!data->m_fired.exchange(true)) {
            data->m_callback();
            delete data;
        }
    };

    if (::timer_create(CLOCK_MONOTONIC, &sev, &data->m_timer_id) != 0) {
        delete data;
        throw std::runtime_error("timer_create failed");
    }

    struct itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(ms.count() / 1000);
    its.it_value.tv_nsec = static_cast<long>((ms.count() % 1000) * 1'000'000);

    if (::timer_settime(data->m_timer_id, 0, &its, nullptr) != 0) {
        ::timer_delete(data->m_timer_id);
        delete data;
        throw std::runtime_error("timer_settime failed");
    }

    return DeadlineHandle{data};
}

void cancelDeadline(DeadlineHandle &handle) noexcept {
    if (handle.m_data) {
        auto *data = static_cast<TimerData*>(handle.m_data);
        ::timer_delete(data->m_timer_id);
        if (!data->m_fired.exchange(true)) {
            delete data;
        }
        handle.m_data = nullptr;
    }
}

}
