module;
#include "pP/Macros.h"
module engine.platform;
import :null;
import std;

namespace pP::detail {

NullPlatform::~NullPlatform() noexcept {
    m_windows.clear();
}

std::expected<std::unique_ptr<IWindow>, int> NullPlatform::createWindow(const WindowDesc& /*desc*/) {
    auto window = std::make_unique<NullWindow>();
    m_windows.push_back(std::move(window));
    return std::make_unique<NullWindow>();
}

} // namespace pP::detail
