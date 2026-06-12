module;
#include "pP/Macros.h"

module engine.core;
import :opaque;
import std;

namespace pP::opaque {

// ------------------------------------------------------------------
// Value are transient, Block is persistent
// ------------------------------------------------------------------

const Block::Value *
Block::Dict::tryGet(const string_literal key) const noexcept {
    for (const auto &[first, second]: *this) {
        if (std::string_view(first.data(), first.size()) == key) {
            return std::addressof(second);
        }
    }
    return nullptr;
}

const Block::Value &
Block::Dict::get(const string_literal key) const noexcept {
    if (const Value *const p_value = tryGet(key)) [[likely]] {
        return *p_value;
    }
    std::unreachable();
}

const Block::Value &
Block::Dict::operator[](const string_literal key) const noexcept {
    return get(key);
}

}
