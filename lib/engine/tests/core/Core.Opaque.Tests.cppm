module;
#include "pP/Macros.h"
export module engine.tests:core_opaque;
import engine.core;
import std;

export namespace pP::tests {
    namespace Opaque {
        namespace Value {
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

                constexpr opaque::Value char_val('x');
                PPR_ASSERT(char_val.get<char>() == 'x');

                constexpr opaque::Value wchar_val(L'x');
                PPR_ASSERT(wchar_val.get<wchar_t>() == L'x');

                constexpr opaque::Value char8_val(u8'x');
                PPR_ASSERT(char8_val.get<char8_t>() == u8'x');

                constexpr opaque::Value i8_val(i8{-1});
                PPR_ASSERT(i8_val.get<i8>() == -1);

                constexpr opaque::Value i16_val(i16{-2});
                PPR_ASSERT(i16_val.get<i16>() == -2);

                constexpr opaque::Value i32_val(i32{-3});
                PPR_ASSERT(i32_val.get<i32>() == -3);

                constexpr opaque::Value u8_val(u8{1});
                PPR_ASSERT(u8_val.get<u8>() == 1);

                constexpr opaque::Value u16_val(u16{2});
                PPR_ASSERT(u16_val.get<u16>() == 2);

                constexpr opaque::Value u32_val(u32{3});
                PPR_ASSERT(u32_val.get<u32>() == 3);
            };

            PPR_UNIT_TEST(decl_arrays) {
                [[maybe_unused]] array_view toto = {1, 2, 3};

                constexpr auto check = [](opaque::Value &&v) {
                    const auto &ar = v.get<opaque::Array>();
                    PPR_ASSERT(ar.size() == 4u);
                    PPR_ASSERT(ar[0].get<bool>() == true);
                    PPR_ASSERT(ar[1].get<i64>() == -1);
                    PPR_ASSERT(ar[2].get<u64>() == 42u);
                    PPR_ASSERT(ar[3].get<std::string_view>() == "ansi");
                };
                check(opaque::Array{true, i64{-1}, u64{42u}, "ansi"});

                std::vector<opaque::Value> promoted = {true, i64{-1}, u64{42u}, "ansi"};
                check(promoted);
            };

            PPR_UNIT_TEST(decl_dict) {
                constexpr auto check = [](opaque::Value &&v) {
                    const auto &map = v.get<opaque::Dict>();
                    PPR_ASSERT(map.size() == 2u);
                    PPR_ASSERT(map[0].second.get<bool>() == false);
                    PPR_ASSERT(map[1].second.get<u64>() == 42u);
                };
                check(opaque::Dict{{"male", false}, {"age", u64{42u}}});

                std::vector<opaque::KeyValue> promoted = {{"male", false}, {"age", u64{42u}}};
                check(promoted);
            };

            inline opaque::Value getValueForDebug() {
                return static_cast<u64>(42u);
            }

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

            PPR_UNIT_TEST(decl_as_nullptr) {
                PPR_ASSERT(opaque::Value{i64{1}}.as<float>() == nullptr);
                PPR_ASSERT(opaque::Value{true}.as<i64>() == nullptr);
                PPR_ASSERT(opaque::Value{"str"}.as<u64>() == nullptr);
                PPR_ASSERT(opaque::Value{3.14}.as<bool>() == nullptr);
            };

            PPR_UNIT_TEST(format_scalar) {
                std::string str;
                str = std::format("{}", opaque::Value{42});
                PPR_ASSERT(str == "42");
                str = std::format("{}", opaque::Value{true});
                PPR_ASSERT(str == "true");
                str = std::format("{}", opaque::Value{3.140});
                PPR_ASSERT(str == "3.14");
                str = std::format("{}", opaque::Value{"hi"});
                PPR_ASSERT(str == R"("hi")");
                str = std::format("{}", opaque::Value{-1.0f});
                PPR_ASSERT(str == "-1");
            };

            PPR_UNIT_TEST(format_array) {
                constexpr auto fmt = [](opaque::Array &&v) -> std::string {
                    return std::format("{}", v);
                };
                const std::string res = fmt({
                    1, true, "ansi", L"wide", 3.14151618, [](opaque::format_context &fmt) {
                        std::format_to(fmt.out(), "the answer is {}", 42);
                    }
                });
                constexpr std::string_view expected = R"EXPECT([1, true, "ansi", "wide", 3.14151618, "the answer is 42"])EXPECT";
                PPR_ASSERT(expected == res);
            };

            PPR_UNIT_TEST(format_object) {
                const auto fmt = [](const opaque::Dict &v) -> std::string {
                    return std::format("{}", v);
                };
                const std::string res = fmt({
                    {"FirstName", "John"},
                    {"LastName", L"Doe"},
                    {"Age", 41u},
                    {"Height", 1.83},
                    {
                        "Hobbies", opaque::Array{
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

        PPR_UNIT_TEST(value) {
            _.recurse(Value::decl_scalars);
            _.recurse(Value::decl_arrays);
            _.recurse(Value::decl_dict);
            _.recurse(Value::decl_formatter);
            _.recurse(Value::decl_as_nullptr);
            _.recurse(Value::format_scalar);
            _.recurse(Value::format_array);
            _.recurse(Value::format_object);
            _.recurse(Value::format_formatter);
        };

        namespace Block {
            PPR_UNIT_TEST(value) {
                using BV = opaque::Block::Value;

                constexpr BV v_bool(true);
                PPR_ASSERT(v_bool.get<bool>() == true);

                constexpr BV v_char('x');
                PPR_ASSERT(v_char.get<char>() == 'x');

                constexpr BV v_wchar(L'x');
                PPR_ASSERT(v_wchar.get<wchar_t>() == L'x');

                constexpr BV v_char8(u8'x');
                PPR_ASSERT(v_char8.get<char8_t>() == u8'x');

                constexpr BV v_i8(i8{-1});
                PPR_ASSERT(v_i8.get<i8>() == -1);

                constexpr BV v_i16(i16{-2});
                PPR_ASSERT(v_i16.get<i16>() == -2);

                constexpr BV v_i32(i32{-3});
                PPR_ASSERT(v_i32.get<i32>() == -3);

                constexpr BV v_i64(i64{-4});
                PPR_ASSERT(v_i64.get<i64>() == -4);

                constexpr BV v_u8(u8{1});
                PPR_ASSERT(v_u8.get<u8>() == 1);

                constexpr BV v_u16(u16{2});
                PPR_ASSERT(v_u16.get<u16>() == 2);

                constexpr BV v_u32(u32{3});
                PPR_ASSERT(v_u32.get<u32>() == 3);

                constexpr BV v_u64(u64{4});
                PPR_ASSERT(v_u64.get<u64>() == 4);

                constexpr BV v_float(0.5f);
                PPR_ASSERT(v_float.get<float>() == 0.5f);

                constexpr BV v_double(1.5);
                PPR_ASSERT(v_double.get<double>() == 1.5);
            };

            PPR_UNIT_TEST(builder_scalars) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                auto test = [&]<typename ArgT>(const opaque::Value &init, ArgT expected) {
                    opaque::Block::Value target;
                    opaque::Block::Builder{target, arena}.dup(init);
                    PPR_ASSERT(target.get<ArgT>() == expected);
                };

                test(opaque::Value{true}, true);
                test(opaque::Value{char{'x'}}, 'x');
                test(opaque::Value{i8{-1}}, i8{-1});
                test(opaque::Value{i16{-2}}, i16{-2});
                test(opaque::Value{i32{-3}}, i32{-3});
                test(opaque::Value{i64{-4}}, i64{-4});
                test(opaque::Value{u8{1}}, u8{1});
                test(opaque::Value{u16{2}}, u16{2});
                test(opaque::Value{u32{3}}, u32{3});
                test(opaque::Value{u64{4}}, u64{4});
                test(opaque::Value{0.5f}, 0.5f);
                test(opaque::Value{1.5}, 1.5);

                arena.restore(mark);
            };

            PPR_UNIT_TEST(builder_strings) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                auto *target = arena.create<opaque::Block::Value>();
                const opaque::Block::Builder builder{*target, arena};

                builder.dup("hello");
                {
                    const auto &rv = target->get<opaque::Block::String>();
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "hello");
                }

                builder.dup(opaque::Value{L"wide"});
                {
                    const auto &rv = target->get<opaque::Block::WString>();
                    PPR_ASSERT(std::wstring_view(rv.data(), rv.size()) == L"wide");
                }

                builder.dup(opaque::Value{u8"utf8"});
                {
                    const auto &rv = target->get<opaque::Block::U8String>();
                    PPR_ASSERT(std::u8string_view(rv.data(), rv.size()) == u8"utf8");
                }

                arena.restore(mark);
            };

            PPR_UNIT_TEST(builder_array) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                auto &target = *arena.create<opaque::Block::Value>();
                opaque::Block::Builder builder{target, arena};

                builder.dup(opaque::Array{true, i64{42}, "str"});

                const auto &arr = target.get<opaque::Block::Array>();
                PPR_ASSERT(arr.size() == 3u);
                PPR_ASSERT(arr[0].get<bool>() == true);
                PPR_ASSERT(arr[1].get<i64>() == 42);
                {
                    const auto &rv = arr[2].get<opaque::Block::String>();
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "str");
                }

                arena.restore(mark);
            };

            PPR_UNIT_TEST(builder_dict) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                const opaque::Block block{
                    {
                        {"name", "ppr"},
                        {"count", u64{42}}
                    },
                    arena
                };

                PPR_ASSERT(block->size() == 2u);
                {
                    const auto &rv = block[0].first;
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "name");
                }
                {
                    const auto &rv = block[0].second.get<opaque::Block::String>();
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "ppr");
                }
                {
                    const auto &rv = block[1].first;
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "count");
                }
                PPR_ASSERT(block[1].second.get<u64>() == 42);

                arena.restore(mark);
            };

            PPR_UNIT_TEST(builder_formatter) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                auto &target = *arena.create<opaque::Block::Value>();
                opaque::Block::Builder builder{target, arena};

                builder.dup([](opaque::format_context &ctx) {
                    return std::format_to(ctx.out(), "fmt {}", 42);
                });
                {
                    const auto &rv = target.get<opaque::Block::String>();
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "fmt 42");
                }

                arena.restore(mark);
            };

            PPR_UNIT_TEST(format_value_scalars) {
                using BV = opaque::Block::Value;

                PPR_ASSERT(std::format("{}", BV{true}) == "true");
                PPR_ASSERT(std::format("{}", BV{char{'x'}}) == "x");
                PPR_ASSERT(std::format("{}", BV{i8{-1}}) == "-1");
                PPR_ASSERT(std::format("{}", BV{i16{-2}}) == "-2");
                PPR_ASSERT(std::format("{}", BV{i32{-3}}) == "-3");
                PPR_ASSERT(std::format("{}", BV{i64{-4}}) == "-4");
                PPR_ASSERT(std::format("{}", BV{u8{1}}) == "1");
                PPR_ASSERT(std::format("{}", BV{u16{2}}) == "2");
                PPR_ASSERT(std::format("{}", BV{u32{3}}) == "3");
                PPR_ASSERT(std::format("{}", BV{u64{4}}) == "4");
                PPR_ASSERT(std::format("{}", BV{0.5f}) == "0.5");
                PPR_ASSERT(std::format("{}", BV{1.5}) == "1.5");
            };

            PPR_UNIT_TEST(format_block) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                opaque::Block block{
                    opaque::Dict{
                        {"name", "ppr"},
                        {"count", u64{42}}
                    },
                    arena
                };

                const auto &dict = *block.m_data;

                // format Block::Value holding a string (exercises string_view overload)
                const std::string val_str = std::format("{}", dict[0].second);
                PPR_ASSERT(val_str == R"("ppr")");

                // format Block::KeyValue (exercises formatter<Block::KeyValue>)
                const std::string kv_str = std::format("{}", dict[0]);
                constexpr std::string_view expected_kv = R"("name": "ppr")";
                PPR_ASSERT(kv_str == expected_kv);

                // format Block::Dict (exercises formatter<Block::Dict>)
                const std::string dict_str = std::format("{}", dict);
                constexpr std::string_view expected_dict = R"({"name": "ppr", "count": 42})";
                PPR_ASSERT(dict_str == expected_dict);

                // format Block (exercises formatter<Block>)
                const std::string block_str = std::format("{}", block);
                PPR_ASSERT(block_str == expected_dict);

                arena.restore(mark);
            };

            PPR_UNIT_TEST(format_complex) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                opaque::Block block{
                    {
                        {"active", true},
                        {"name", "test"},
                        {"count", u64{7}},
                        {"tags", opaque::Array{"a", "b", "c"}},
                        {"meta", opaque::Dict{{"x", 1.5}}}
                    },
                    arena
                };

                const std::string result = std::format("{}", block);
                constexpr std::string_view expected =
                        R"({"active": true, "name": "test", "count": 7, "tags": ["a", "b", "c"], "meta": {"x": 1.5}})";
                PPR_ASSERT(result == expected);

                arena.restore(mark);
            };

            PPR_UNIT_TEST(constructor) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                const opaque::Block block{
                    {
                        {"bool", true},
                        {"int", i64{-99}},
                        {"uint", u64{42}},
                        {"float", 3.14},
                        {"str", "hello"}
                    },
                    arena
                };

                const auto &dict = *block.m_data;
                PPR_ASSERT(dict.size() == 5u);

                auto find = [&](const char *key) -> const opaque::Block::Value & {
                    for (u32 i = 0; i < dict.size(); ++i) {
                        const auto &k = dict[i].first;
                        if (std::string_view(k.data(), k.size()) == key) {
                            return dict[i].second;
                        }
                    }
                    PPR_ASSERT(!"key not found");
                    static opaque::Block::Value fallback{};
                    return fallback;
                };

                PPR_ASSERT(find("bool").get<bool>() == true);
                PPR_ASSERT(find("int").get<i64>() == -99);
                PPR_ASSERT(find("uint").get<u64>() == 42);
                PPR_ASSERT(find("float").get<double>() == 3.14);
                {
                    const auto &rv = find("str").get<opaque::Block::String>();
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "hello");
                }

                arena.restore(mark);
            };

            PPR_UNIT_TEST(constructor_with_generator_fmt) {
                auto arena = mem::Allocator<mem::ScratchPad>{};
                const auto mark = arena.watermark();

                const opaque::Block block{
                    opaque::Dict{
                        {
                            "int", 99
                        },
                        {
                            "fmt", [](opaque::format_context &ctx) {
                                return std::format_to(ctx.out(), "formatted");
                            }
                        },
                        {
                            "arr", opaque::Array{1,true,"string",3.1415f},
                        }
                    },
                    arena
                };

                const auto &dict = *block.m_data;
                PPR_ASSERT(dict.size() == 3u);

                const auto *const p_value = dict.tryGet("int");
                PPR_ASSERT(p_value != nullptr);
                PPR_ASSERT(p_value->get<int>() == 99);
                {
                    const auto &rv = dict["fmt"].get<opaque::Block::String>();
                    PPR_ASSERT(std::string_view(rv.data(), rv.size()) == "formatted");
                }

                arena.restore(mark);
            };
        }

        PPR_UNIT_TEST(block) {
            _.recurse(Block::value);
            _.recurse(Block::builder_scalars);
            _.recurse(Block::builder_strings);
            _.recurse(Block::builder_array);
            _.recurse(Block::builder_dict);
            _.recurse(Block::builder_formatter);
            _.recurse(Block::format_value_scalars);
            _.recurse(Block::format_block);
            _.recurse(Block::format_complex);
            _.recurse(Block::constructor);
            _.recurse(Block::constructor_with_generator_fmt);
        };
    }

    PPR_UNIT_TEST(opaque) {
        _.recurse(Opaque::value);
        _.recurse(Opaque::block);
    };
}
