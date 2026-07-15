module;
#include "pP/Macros.h"
export module engine.tests.core:enums;
import engine.core;
import std;

export namespace pP::tests {
    namespace Enums {
        enum class Color : u32 {
            none = 0u,
            red = 0xFF0000u,
            green = 0x00FF00u,
            blue = 0x0000FFu,
            all = red | green | blue,
        };

        static_assert(pP::details::TEnumFlags<Color>);

        enum class Flags : u32 {
            none = 0u,
            a = 1u << 0,
            b = 1u << 1,
            c = 1u << 2,
            ab = a|b,
            all = a|b|c
        };

        static_assert(pP::details::TEnumFlags<Flags>);

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
            const auto a = Flags::a & Flags::all;
            PPR_ASSERT(enumOrd(a) == enumOrd(Flags::a));

            const auto none = Flags::a & Flags::b;
            PPR_ASSERT(enumOrd(none) == enumOrd(Flags::none));
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
            const auto expr = ((Flags::all ^ Flags::c) & Flags::a) | Flags::b;
            PPR_ASSERT(enumOrd(expr) == enumOrd(Flags::ab));
        };

        PPR_UNIT_TEST(open_flags_or) {
            constexpr auto read_flag = hal::io::OpenFlags{hal::io::OpenFlags::read};
            constexpr auto write_flag = hal::io::OpenFlags{hal::io::OpenFlags::write};
            constexpr auto rw = read_flag | write_flag;
            PPR_ASSERT(rw == hal::io::OpenFlags{hal::io::OpenFlags::read | hal::io::OpenFlags::write});
        };
    }

    PPR_UNIT_TEST(enums) {
        _.recurse({
            Enums::ord,
            Enums::flags_any,
            Enums::flags_and,
            Enums::flags_or,
            Enums::flags_xor,
            Enums::flags_mixed_operations,
            Enums::open_flags_or,
        });
    };
}
