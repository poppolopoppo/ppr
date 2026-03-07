module;
#include "pP/Macros.h"
export module engine.tests:core_enums;
import engine.core;
import std;

export namespace pP::tests {
    namespace Enums {
        enum class Color : u32 {
            red = 0xFF0000u,
            green = 0x00FF00u,
            blue = 0x0000FFu,
        };

        constexpr bool is_enum_flags(std::type_identity<Color>) noexcept {
            return true;
        }

        enum class Flags : u32 {
            none = 0u,
            a = 1u << 0,
            b = 1u << 1,
            c = 1u << 2,
        };

        constexpr bool is_enum_flags(std::type_identity<Flags>) noexcept {
            return true;
        }

        PPR_UNIT_TEST(ord) {
            PPR_ASSERT(enumOrd(Color::red) == 0xFF0000u);
            PPR_ASSERT(enumOrd(Color::green) == 0x00FF00u);
            PPR_ASSERT(enumOrd(Color::blue) == 0x0000FFu);
        };

        PPR_UNIT_TEST(flags_any) {
            PPR_ASSERT(any(Flags::a));
            PPR_ASSERT(any(Flags::b));
            PPR_ASSERT(any(Flags::c));
            PPR_ASSERT(!any(Flags::none));
        };

        PPR_UNIT_TEST(flags_and) {
            const auto ab = Flags::a & Flags::b;
            PPR_ASSERT(enumOrd(ab) == ((1u << 0) | (1u << 1)));

            const auto abc = Flags::a & Flags::b & Flags::c;
            PPR_ASSERT(enumOrd(abc) == ((1u << 0) | (1u << 1) | (1u << 2)));
        };

        PPR_UNIT_TEST(flags_or) {
            const auto a_or_b = Flags::a | Flags::b;
            PPR_ASSERT(enumOrd(a_or_b) == ((1u << 0) | (1u << 1)));

            const auto abc = Flags::a | Flags::b | Flags::c;
            PPR_ASSERT(enumOrd(abc) == ((1u << 0) | (1u << 1) | (1u << 2)));
        };

        PPR_UNIT_TEST(flags_xor) {
            const auto a_xor_b = Flags::a ^ Flags::b;
            PPR_ASSERT(enumOrd(a_xor_b) == ((1u << 0) | (1u << 1)));

            const auto a_xor_a = Flags::a ^ Flags::a;
            PPR_ASSERT(enumOrd(a_xor_a) == 0u);
        };

        PPR_UNIT_TEST(flags_mixed_operations) {
            const auto expr = (Flags::a | Flags::b) & Flags::c ^ Flags::a;
            PPR_ASSERT(enumOrd(expr) == ((1u << 1) | (1u << 2)));
        };
    }

    PPR_UNIT_TEST(enums) {
        _.recurse(Enums::ord);
        _.recurse(Enums::flags_any);
        _.recurse(Enums::flags_and);
        _.recurse(Enums::flags_or);
        _.recurse(Enums::flags_xor);
        _.recurse(Enums::flags_mixed_operations);
    };
}
