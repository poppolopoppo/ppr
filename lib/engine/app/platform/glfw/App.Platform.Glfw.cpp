module;
#include "pP/Macros.h"
#include "App.Platform.Glfw.include.hpp"

module engine.app;

import :frame.packet;
import :platform.glfw;
import :platform.glfw.input;
import :platform.glfw.player;
import :platform.glfw.window;
import std;
import engine.core;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(GlfwPlatform, info, none)

    namespace {
        // ora-25 measurement-only probe writer: one side-channel event per
        // collection/wait/dispatch step on the monotonic backing clock read
        // fresh at each probe boundary (time::now(), never the latched
        // TimerManager::now()). The tick AND the batch identity ride
        // explicit parameters (never an implicit read inside logic or
        // inside this writer). detail_a carries the input-batch identity:
        // the pending event count (PendingInputBatch::size(), i.e. last
        // per-event index + 1) at collection, the dispatched count at
        // consumption — collection stamps accumulate across pumps and the
        // consumption stamp carries the total, so input age joins
        // collection to actual consumption by count continuity. noexcept,
        // allocation-free, never touches control flow.
        void recordPlatformTelemetry_(
            const TimePoint tick, const EFrameTelemetryEvent kind, const i64 detail_a = 0, const i64 detail_b = 0,
            const int host_outcome = 0) noexcept {
            FrameTelemetryEvent event{};
            event.m_kind = kind;
            event.m_cpu_tick_ns = frameTelemetryCpuTickNs(tick);
            event.m_detail_a = detail_a;
            event.m_detail_b = detail_b;
            event.m_host_outcome = host_outcome;
            event.m_thread_tag = 0u;
            frameTelemetryMainBuffer().record(event);
        }
    }

    namespace platform {
        class GlfwErrorCategory : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "glfw";
            }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<errc>(ev)) {
                    case errc::ok: return "indicates success";
                    case errc::fail:
                        return "generic failure code - meaning a serious error occurred and the call couldn't complete";
                    case errc::initialization_failed: return "indicates that the plaform failed to initialize";
                    case errc::invalid_argument:
                        return "indicates that an argument passed in as parameter to a method is invalid";

                    default: return std::format("unknown glfw result ({})", ev);
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                switch (static_cast<errc>(ev)) {
                    case errc::invalid_argument: return std::errc::invalid_argument;
                    default: return {ev, *this};
                }
            }
        };

        static const GlfwErrorCategory g_glfw_error_category{};

        [[nodiscard]] const std::error_category &error_category() noexcept {
            return g_glfw_error_category;
        }

        [[nodiscard]] std::error_code make_error_code(const int result) noexcept {
            return result == GL_TRUE ? std::error_code{}
                                     : std::error_code{static_cast<int>(errc::fail), g_glfw_error_category};
        }

        [[nodiscard]] std::error_code make_error_code(const errc error_code) noexcept {
            return make_error_code(enumOrd(error_code));
        }
    }

    std::unique_ptr<IPlatform> IPlatform::create() noexcept {
        return std::make_unique<GlfwPlatform>();
    }

    GlfwPlatform::GlfwPlatform() noexcept = default;

    safe_ptr<Application> GlfwPlatform::getApplication() const noexcept {
        return m_application;
    }

    safe_ptr<IInputService> GlfwPlatform::getInputService() const noexcept {
        return safe_ptr(m_input_service.get());
    }

    safe_ptr<IWindowService> GlfwPlatform::getWindowService() const noexcept {
        return safe_ptr(m_window_service.get());
    }

    safe_ptr<IPlayerService> GlfwPlatform::getPlayerService() const noexcept {
        return safe_ptr(m_player_service.get());
    }

    std::string_view GlfwPlatform::getPlatformName() const noexcept {
        return "GLFW";
    }

    platform::Version GlfwPlatform::getPlatformVersion() const noexcept {
        int major = 0, minor = 0, revision = 0;
        ::glfwGetVersion(&major, &minor, &revision);
        return {.m_major = major, .m_minor = minor, .m_revision = revision};
    }

    std::error_code GlfwPlatform::initialize(Application &app) {
        PPR_LOG(GlfwPlatform, info, "initialize GLFW platform", {{"version", ::glfwGetVersionString()}});

        PPR_ASSERT(not m_application.isValid());
        m_application.reset(&app);

        ::glfwSetErrorCallback([](int error_code, const char *description) {
            PPR_LOG(GlfwPlatform, error, "GLFW error", {{"error_code", error_code}, {"description", description}});
        });

        constexpr ::GLFWallocator glfw_allocator{
            .allocate = [](size_t size, void *) -> void * { return std::malloc(size); },
            .reallocate = [](void *block, size_t size, void *) -> void * { return std::realloc(block, size); },
            .deallocate = [](void *block, void *) { std::free(block); },
            .user = nullptr,
        };
        // Intentional process-lifetime state: GLFW offers no API to clear a
        // custom allocator, so these hooks stay installed on every exit path.
        ::glfwInitAllocator(&glfw_allocator);

        if (not ::glfwInit()) [[unlikely]] {
            PPR_LOG(GlfwPlatform, error, "failed to init GLFW");
            ::glfwSetErrorCallback(nullptr);
            return platform::errc::initialization_failed;
        }

        // Rolls back every stage committed so far, then always releases GLFW
        // itself; returns the stage error that triggered the unwind.
        bool success = false;
        PPR_DEFER {
            if (not success) [[unlikely]] {
                std::ignore = shutdown(app);
            }
        };

        const ApplicationDomain domain = app.getDomain();
        ServicesStore &app_services = app.getServices();

        if (not domain.m_is_headless) {
            m_window_service = std::make_unique<GlfwWindow>();
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, m_window_service->initialize(app));
            app_services.insert_or_assign<IWindowService>(m_window_service.get());
        }

        if (domain.m_is_interactive) {
            m_input_service = std::make_unique<GlfwInput>();
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, m_input_service->initialize());
            app_services.insert_or_assign<IInputService>(m_input_service.get());
        }

        if (domain.m_needs_presence) {
            m_player_service = std::make_unique<GlfwPlayer>();
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, m_player_service->initialize(*m_input_service));
            app_services.insert_or_assign<IPlayerService>(m_player_service.get());
        }

        success = true;
        return default_value_v;
    }

    std::error_code GlfwPlatform::shutdown(Application &app) {
        PPR_LOG(GlfwPlatform, info, "shutdown GLFW platform", {{"version", ::glfwGetVersionString()}});

        // Idempotent: Application::shutdown() calls here on every teardown,
        // and initialize() already shuts down on failure, so a second call
        // must not re-fire the application-identity assert below.
        if (not m_application.isValid()) {
            return default_value_v;
        }
        PPR_ASSERT(m_application == &app);
        PPR_DEFER {
            m_application.reset();
        };

        ServicesStore &app_services = app.getServices();

        std::error_code first_err{};
        if (m_player_service) {
            PPR_VERIFY(app_services.erase<IPlayerService>(*m_player_service));
            PPR_RETAIN_ERROR_ON_FAIL(GlfwPlatform, first_err, m_player_service->shutdown());
            m_player_service.reset();
        }

        if (m_input_service) {
            PPR_VERIFY(app_services.erase<IInputService>(*m_input_service));
            PPR_RETAIN_ERROR_ON_FAIL(GlfwPlatform, first_err, m_input_service->shutdown());
            m_input_service.reset();
        }

        if (m_window_service) {
            PPR_VERIFY(app_services.erase<IWindowService>(*m_window_service));
            PPR_RETAIN_ERROR_ON_FAIL(GlfwPlatform, first_err, m_window_service->shutdown());
            m_window_service.reset();
        }

        ::glfwTerminate();
        ::glfwSetErrorCallback(nullptr);
        return default_value_v;
    }

    std::error_code GlfwPlatform::pumpNativeEvents() {
        // Collection only: window-event callbacks stage owned records into
        // the input batch; no client callback runs until update() dispatches
        // the completed batches below. Repeatable within a frame.
        // ora-25: active drain half of the wait/drain split (see
        // waitNativeEvents below); complements the outer
        // PPR.Frame.ServicePlatform scope owned by Application.
        const hal::ProfileScope scope_collect{"PPR.Frame.PlatformCollect"};
        // Pending count before this pump (busy-frame carryover, 0 when no
        // input service); post-pump size surfaces at the next stamp.
        const i64 collect_pending = m_input_service != nullptr
                                        ? static_cast<i64>(m_input_service->m_native_batch.size())
                                        : 0;
        recordPlatformTelemetry_(time::now(), EFrameTelemetryEvent::platform_collect, collect_pending);
        GlfwInput *const p_inputs = m_input_service.get();
        if (p_inputs != nullptr) [[likely]] {
            p_inputs->setNativeBatchCollection(true);
        }
        PPR_DEFER {
            if (p_inputs != nullptr) {
                p_inputs->setNativeBatchCollection(false);
            }
        };

        if (GlfwWindow *const p_windows = m_window_service.get()) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_windows->pollEvents());
        }

        return default_value_v;
    }

    std::error_code GlfwPlatform::waitNativeEvents(const TimeSpan timeout) {
        // Event-driven sleep: the timed GLFW wait services arrivals on
        // wake (or times out), then one collection pump stages them into
        // the owned batch. Device advancement still happens only in
        // update(); this path only collects, like pumpNativeEvents.
        // The timeout is a REQUESTED bound (best-effort OS wait: an event,
        // postEmptyEvent, or exit/credit/config wake ends it early, and
        // scheduling granularity may end it late — never an exact
        // wall-clock guarantee). Callers pass the recomputed remainder of
        // their absolute deadline, never a stale pre-service slice; a
        // non-positive remainder degrades to a single collection pump and
        // never blocks.
        // Collection guard covers BOTH native calls below: glfwWaitEvents
        // Timeout and poll both execute window/input callbacks, and the
        // immediate path (GlfwInput::post*, focus hotplug) dispatches to
        // clients unless collection mode stages them. Exception-safe via
        // the defer so an early error return cannot leak the flag.
        if (timeout <= TimeSpan{}) {
            return pumpNativeEvents();
        }
        GlfwInput *const p_inputs = m_input_service.get();
        if (p_inputs != nullptr) [[likely]] {
            p_inputs->setNativeBatchCollection(true);
        }
        PPR_DEFER {
            if (p_inputs != nullptr) {
                p_inputs->setNativeBatchCollection(false);
            }
        };

        if (GlfwWindow *const p_windows = m_window_service.get()) [[likely]] {
            // ora-25 wait/drain split: the intentional native wait and the
            // active collection drain carry distinct scopes AND distinct
            // side-channel stamps, so Phase 3 can attribute parked time vs
            // collection work (the outer PPR.Frame.ServicePlatform scope in
            // Application encloses both and is left untouched).
            {
                const hal::ProfileScope scope_wait{"PPR.Frame.PlatformWait"};
                recordPlatformTelemetry_(time::now(), EFrameTelemetryEvent::platform_wait_begin);
                PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_windows->waitEventsTimeout(timeout));
                recordPlatformTelemetry_(time::now(), EFrameTelemetryEvent::platform_wait_end);
            }
            {
                const hal::ProfileScope scope_collect{"PPR.Frame.PlatformCollect"};
                const i64 wait_collect_pending = m_input_service != nullptr
                                                     ? static_cast<i64>(m_input_service->m_native_batch.size())
                                                     : 0;
                recordPlatformTelemetry_(time::now(), EFrameTelemetryEvent::platform_collect, wait_collect_pending);
                PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_windows->pollEvents());
            }
        } else {
            // Headless: paced slice with no native queue to service.
            if (timeout > TimeSpan{}) {
                std::this_thread::sleep_for(timeout);
            }
        }

        return default_value_v;
    }

    void GlfwPlatform::postEmptyEvent() noexcept {
        // Any-thread relay; safe before init and after shutdown (no window
        // service yet/anymore means nobody is parked in the native wait).
        if (GlfwWindow *const p_windows = m_window_service.get()) {
            p_windows->postEmptyEvent();
        }
    }

    std::error_code GlfwPlatform::update(const TimeSpan dt) {
        const hal::ProfileScope scope{"App.Platform.Update"};
        // Single client advancement per frame, in order: device-poll first
        // (device time, transient clears, once-per-frame gamepad sampling),
        // then native collection into owned batches, then exactly-once batch
        // dispatch at this defined update point. Extra native pumps within
        // the frame reuse pumpNativeEvents(), never this path, so
        // press/release pairs and posted text survive.
        // Clear transient input state BEFORE window-event dispatch appends this
        // frame's characters. Safe to run first: device polling is event-pump
        // independent (gamepads poll via glfwGetGamepadState, keyboard/mouse polls
        // only reset transient
        // state), and dispatch below then uses this frame's delta time.
        if (GlfwInput *const p_inputs = m_input_service.get()) [[likely]] {
            const hal::ProfileScope scope_poll{"PPR.Frame.PlatformDevicePoll"};
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_inputs->pollInputDevices(dt));
            // ora-25: device-poll stamp (all collection paths stamped),
            // fresh at the poll boundary with the still-pending count.
            recordPlatformTelemetry_(
                time::now(), EFrameTelemetryEvent::platform_device_poll,
                static_cast<i64>(p_inputs->m_native_batch.size()));
        }

        PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, pumpNativeEvents());

        if (GlfwInput *const p_inputs = m_input_service.get()) [[likely]] {
            // Deferred client halves for focus + hotplug run inside this
            // exactly-once dispatch, in collection order with the input
            // replay: the platform halves (flags, registries) already ran at
            // collection time above.
            auto focus_applier = [this](const safe_ptr<Window> window, const bool focused) -> std::error_code {
                if (not window.isValid()) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                // B2'c: deliver the CAPTURED value to BOTH subscribers —
                // never re-read the live flag, which may already reflect a
                // later collected arrival queued behind this record.
                window->m_when_focused(*window, focused);
                if (GlfwWindow *const p_windows = m_window_service.get()) {
                    PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_windows->notifyWindowFocused(*window, focused));
                }
                return default_value_v;
            };
            auto device_applier =
                [p_inputs](const safe_ptr<const IInputDevice> device, const bool connected) -> std::error_code {
                if (not device.isValid()) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                if (connected) {
                    return p_inputs->notifyDeviceConnected(*device);
                }
                return p_inputs->notifyDeviceDisconnected(*device);
            };
            // Batch identity captured BEFORE the exactly-once dispatch
            // swaps/clears it: the dispatched count (per-event indices
            // 0..count-1) joins against the accumulated collection stamps
            // above by count continuity.
            const i64 dispatch_count = static_cast<i64>(p_inputs->m_native_batch.size());
            const std::error_code dispatch_err = p_inputs->dispatchCollectedBatches(
                std::move(focus_applier), std::move(device_applier));
            // ora-25: input-consumption stamp at the exactly-once dispatch
            // point, including failed dispatches (outcome carried, control
            // flow unchanged).
            recordPlatformTelemetry_(
                time::now(), EFrameTelemetryEvent::input_consumed, dispatch_count, 0,
                dispatch_err ? dispatch_err.value() : 0);
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, dispatch_err);
        }

        return default_value_v;
    }
}
