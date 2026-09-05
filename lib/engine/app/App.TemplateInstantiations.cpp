module;
#include "pP/Macros.h"

module engine.app;
import engine.core;
import engine.math;

import :service.window;
import :service.input;
import :service.player;
import :window.handle;

template class pP::Delegate<void (const pP::Window &)>;
template class pP::Delegate<void (const pP::Window &, bool)>;
template class pP::Delegate<void (const pP::Window &, const pP::int2 &)>;
template class pP::Delegate<void (const pP::Window &, const pP::float2 &)>;
template class pP::Delegate<void (const pP::Window &, pP::EKeyboardKey)>;
template class pP::Delegate<void (const pP::Window &, pP::EKeyboardKey, bool)>;
template class pP::Delegate<void (const pP::Window &, pP::EMouseButton, bool)>;
template class pP::Delegate<void (const pP::Window &, pP::hal::native::char_t)>;

template class pP::BroadcastCallback<std::error_code (const pP::Monitor &)>;
template class pP::BroadcastCallback<std::error_code (const pP::Window &)>;
template class pP::BroadcastCallback<std::error_code (const pP::Window &, bool)>;
template class pP::BroadcastCallback<std::error_code (const pP::Window &, const pP::float2 &)>;

template class pP::BroadcastCallback<std::error_code (pP::IInputDevice const &)>;
template class pP::BroadcastCallback<std::error_code (pP::InputActionEvent const &, pP::InputKey const &)>;
template class pP::BroadcastCallback<std::error_code (pP::InputKey const &)>;
template class pP::BroadcastCallback<std::error_code (pP::TimeSpan)>;

template class pP::BroadcastCallback<std::error_code (pP::Player const &)>;
