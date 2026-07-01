module;
#include "pP/Macros.h"
module engine.platform;
import :null;
import std;

namespace pP::detail {

std::expected<std::unique_ptr<IWindow>, int> NullPlatform::createWindow(const WindowDesc& /*desc*/) {
    return std::make_unique<NullWindow>();
}

} // namespace pP::detail
