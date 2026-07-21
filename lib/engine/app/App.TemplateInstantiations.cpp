module;
#include "pP/Macros.h"

module engine.app;
import engine.core;
import engine.math;
import :service.window;
import :service.input;
import :service.player;
import :window.handle;

template class pP::DeferredCallback<std::error_code (pP::Monitor const &)>;
template class pP::DeferredCallback<std::error_code (pP::Window const &)>;
template class pP::DeferredCallback<std::error_code (pP::Window const &, bool)>;
template class pP::DeferredCallback<std::error_code (pP::Window const &, pP::int2 const &)>;
template class pP::DeferredCallback<std::error_code (pP::Window const &, pP::float2 const &)>;
template class pP::DeferredCallback<std::error_code (pP::Window const &, pP::int2)>;
template class pP::DeferredCallback<std::error_code (pP::Window const &, pP::float2)>;
template class pP::Callback<std::error_code (pP::IInputService const &, pP::IInputDevice const &)>;
template class pP::Callback<std::error_code (pP::IInputService const &, pP::InputActionEvent const &, pP::InputKey const &)>;
template class pP::Callback<std::error_code (pP::IInputService const &, pP::InputKey const &)>;
template class pP::Callback<std::error_code (pP::IInputService const &, pP::TimeSpan)>;
template class pP::Callback<std::error_code (pP::IPlayerService const &, pP::Player const &)>;
