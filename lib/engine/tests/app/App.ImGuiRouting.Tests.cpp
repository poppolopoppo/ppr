module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    // Routing order: an ImGui-style listener (raw callback only, no mappings)
    // at -1000 on the window context still observes a scene-mapped key before
    // the scene listener at 0 consumes it. Mirrors Application init order
    // (ImGui registered first) without instantiating the RHI-backed service.
    PPR_UNIT_TEST (imgui_first_on_window_context_observes_scene_mapped_key) {
        // Listeners outlive the context: it holds safe_ptrs to them.
        InputListener imgui_listener{};
        int raw_count = 0;
        imgui_listener.setRawKeyCallback([&](const TimeSpan, const InputMessage &) noexcept {
            ++raw_count;
            return EInputMessageResponse::unhandled;
        });

        InputAction scene_action{"SceneMove", EInputValueType::digital, EInputActionFlags::none};
        InputMapping scene_mapping{"Scene"};
        scene_mapping.mapInputKey(SharedInputAction{&scene_action}, InputKey::w);
        InputListener scene_listener{};
        scene_listener.addInputMapping(SharedInputMapping{&scene_mapping}, 0);
        int scene_triggered = 0;
        scene_action.setTriggered([&scene_triggered](const InputActionEvent &, const InputKey &) noexcept {
            ++scene_triggered;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&imgui_listener}, -1000);
        window_context.addInputListener(safe_ptr<InputListener>{&scene_listener}, 0);

        const InputMessage msg{
            InputKey::w,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const EInputMessageResponse response = window_context.postKeyEvent(TimeSpan{}, msg);

        PPR_TEST_ASSERT(raw_count == 1);
        PPR_TEST_ASSERT(scene_triggered == 1);
        PPR_TEST_ASSERT(response == EInputMessageResponse::consumed);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest imgui_routing = UnitTest::Named("imgui_routing") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::imgui_first_on_window_context_observes_scene_mapped_key,
        });
    };
} // namespace pP::tests
