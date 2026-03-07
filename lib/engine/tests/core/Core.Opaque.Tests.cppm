module;
#include "pP/Macros.h"
export module engine.tests:core_opaque;
import engine.core;
import std;

export namespace pP::tests {
    namespace Opaque {
        PPR_UNIT_TEST(decl_scalars) {
            constexpr opaque::Value boolean(true);
            PPR_ASSERT(boolean.get<bool>() == true);

            constexpr opaque::Value signed_int(i64{-1});
            PPR_ASSERT(signed_int.get<i64>() == -1);

            constexpr opaque::Value unsigned_int(u64{42u});
            PPR_ASSERT(unsigned_int.get<u64>() == 42u);

            constexpr opaque::Value single_fp(0.0f);
            PPR_ASSERT(single_fp.get<float>() == 0.0f);

            constexpr opaque::Value double_fp(1.0);
            PPR_ASSERT(double_fp.get<double>() == 1.0);

            constexpr opaque::Value ansi_str("ansi");
            PPR_ASSERT(ansi_str.get<std::string_view>() == "ansi");

            constexpr opaque::Value wide_str(L"wide");
            PPR_ASSERT(wide_str.get<std::wstring_view>() == L"wide");

            constexpr opaque::Value utf8_str(u8"utf8");
            PPR_ASSERT(utf8_str.get<std::u8string_view>() == u8"utf8");
        };

        PPR_UNIT_TEST(decl_arrays) {
            constexpr auto check = [](opaque::Value &&v) {
                const auto ar = v.get<opaque::Array>();
                PPR_ASSERT(ar.size() == 4u);
                PPR_ASSERT(ar[0].get<bool>() == true);
                PPR_ASSERT(ar[1].get<i64>() == -1);
                PPR_ASSERT(ar[2].get<u64>() == 42u);
                PPR_ASSERT(ar[3].get<std::string_view>() == "ansi");
            };
            check({true, i64{-1}, u64{42u}, "ansi"});

            opaque::Value promoted = {true, i64{-1}, u64{42u}, "ansi"};
            check(std::move(promoted));
        };

        PPR_UNIT_TEST(decl_dict) {
            constexpr auto check = [](opaque::Value &&v) {
                const auto map = v.get<opaque::Dict>();
                PPR_ASSERT(map.size() == 2u);
                PPR_ASSERT(map[0].value.get<bool>() == false);
                PPR_ASSERT(map[1].value.get<u64>() == 42u);
            };
            check(opaque::Dict{{"male", false}, {"age", u64{42u}}});

            opaque::Value promoted = opaque::Dict{{"male", false}, {"age", u64{42u}}};
            check(std::move(promoted));
        };

        inline opaque::Value getValueForDebug() {
            return u64(42u);
        }

        PPR_UNIT_TEST(decl_fn) {
            constexpr auto check = [](opaque::Value &&v) {
                const opaque::Value r = v.get<opaque::Fn>()();
                PPR_ASSERT(r.get<u64>() == 42u);
            };
            opaque::Fn static_fn(&getValueForDebug);
            check(static_fn);
            check(&getValueForDebug);
            opaque::Fn var_fn([]() -> opaque::Value {
                return u64{42u};
            });
            check(var_fn);
            check([]() -> opaque::Value {
                return u64{42u};
            });
        };

        PPR_UNIT_TEST(decl_formatter) {
            constexpr auto check = [](opaque::Value &&v) {
                PPR_ASSERT(v.as<opaque::Formatter>() != nullptr);
            };
            check(opaque::Formatter([](opaque::format_context &ctx) -> decltype(auto) {
                return ctx.out();
            }));
            check([](opaque::format_context &ctx) -> decltype(auto) {
                return ctx.out();
            });
            constexpr auto formatter = [](opaque::format_context &ctx) {
                return ctx.out();
            };
            check(opaque::Formatter(formatter));
        };

        PPR_UNIT_TEST(format_array) {
            constexpr auto fmt = [](opaque::Array &&v) -> std::string {
                return std::format("{}", v);
            };
            const std::string res = fmt({
                1, true, "ansi", L"wide", 3.14151618, []() -> opaque::Value {
                    return u8"utf8";
                }
            });
            constexpr std::string_view expected = R"EXPECT([1, true, "ansi", "wide", 3.14151618, "utf8"])EXPECT";
            PPR_ASSERT(expected == res);
        };

        PPR_UNIT_TEST(format_object) {
            const auto fmt = [](opaque::Dict &&v) -> std::string {
                return std::format("{}", v);
            };
            const std::string res = fmt({
                {"FirstName", "John"},
                {"LastName", L"Doe"},
                {"Age", 41u},
                {"Height", 1.83},
                {
                    "Hobbies", {
                        "coding", "gaming", "joking"
                    }
                }
            });
            constexpr std::string_view expected =
                    R"EXPECT({"FirstName": "John", "LastName": "Doe", "Age": 41, "Height": 1.83, "Hobbies": ["coding", "gaming", "joking"]})EXPECT";
            PPR_ASSERT(expected == res);
        };

        PPR_UNIT_TEST(format_formatter) {
            constexpr auto fmt = [](opaque::Value &&v) -> std::string {
                return std::format("{}", v);
            };
            const std::string res = fmt([](opaque::format_context &ctx) {
                return std::format_to(ctx.out(), "This is {:02} formatted {}", 1, "text");
            });
            constexpr std::string_view expected = R"EXPECT("This is 01 formatted text")EXPECT";
            PPR_ASSERT(expected == res);
        };
    }

    PPR_UNIT_TEST(opaque) {
        _.recurse(Opaque::decl_scalars);
        _.recurse(Opaque::decl_arrays);
        _.recurse(Opaque::decl_dict);
        _.recurse(Opaque::decl_fn);
        _.recurse(Opaque::decl_formatter);
        _.recurse(Opaque::format_array);
        _.recurse(Opaque::format_object);
        _.recurse(Opaque::format_formatter);
    };
}
