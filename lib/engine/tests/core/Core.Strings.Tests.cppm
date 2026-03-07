module;
#include "pP/Macros.h"
export module engine.tests:core_strings;
import engine.core;
import std;

export namespace pP::tests {
    namespace Strings {
        namespace Helpers {
            PPR_UNIT_TEST(char_helpers) {
                PPR_ASSERT(toLower('A') == 'a');
                PPR_ASSERT(toLower('z') == 'z');
                PPR_ASSERT(toLower('!') == '!');

                PPR_ASSERT(toUpper('a') == 'A');
                PPR_ASSERT(toUpper('Z') == 'Z');
                PPR_ASSERT(toUpper('!') == '!');

                PPR_ASSERT(isSpace(' '));
                PPR_ASSERT(isSpace('\n'));
                PPR_ASSERT(isSpace('\t'));
                PPR_ASSERT(!isSpace('a'));

                PPR_ASSERT(isDigit('0') && isDigit('9'));
                PPR_ASSERT(!isDigit('a'));

                PPR_ASSERT(isAlpha('a') && isAlpha('Z'));
                PPR_ASSERT(!isAlpha('1'));

                PPR_ASSERT(isAlnum('a') && isAlnum('5'));
                PPR_ASSERT(!isAlnum(' '));
            };

            PPR_UNIT_TEST(escape_functions) {
                constexpr auto n = charEscape('\n');
                PPR_ASSERT(n.view() == "\\n");

                constexpr auto quote = charEscape('"');
                PPR_ASSERT(quote.view() == "\\\"");

                constexpr auto normal = charEscape('x');
                PPR_ASSERT(normal.view() == "x");

                constexpr auto amp = xmlEscapeChar('&');
                PPR_ASSERT(amp.view() == "&amp;");

                constexpr auto lt = xmlEscapeChar('<');
                PPR_ASSERT(lt.view() == "&lt;");

                constexpr auto normal_xml = xmlEscapeChar('x');
                PPR_ASSERT(normal_xml.view() == "x");
            };

            PPR_UNIT_TEST(case_fold_char) {
                constexpr CaseFoldChar c('A');
                PPR_ASSERT(*c == 'a');
                PPR_ASSERT(c == 'a');
                PPR_ASSERT(c == CaseFoldChar('a'));
                PPR_ASSERT(c <=> 'b' == std::strong_ordering::less);
            };
        }

        PPR_UNIT_TEST(helpers) {
            _.recurse(Helpers::char_helpers);
            _.recurse(Helpers::escape_functions);
            _.recurse(Helpers::case_fold_char);
        };

        namespace Literal {
            PPR_UNIT_TEST(static_interop) {
                constexpr string_literal lit = "test";
                constexpr static_string stat = "test";

                PPR_ASSERT(lit == stat);
                PPR_ASSERT(lit.view() == stat.view());
                PPR_ASSERT(hashValue(lit) == hashValue(stat));

                constexpr auto sub = stat.subrange<1, 3>();
                PPR_ASSERT(sub.view() == "est");
            };

            PPR_UNIT_TEST(basic_view_and_equality) {
                PPR_ASSERT(details::relocatable<string_literal>::value);
                constexpr string_literal a("hello");
                constexpr string_literal b("hello");
                PPR_ASSERT(a == b);
                constexpr auto sv = a.view();
                PPR_ASSERT(sv == "hello");
            };

            PPR_UNIT_TEST(empty_string) {
                constexpr static_string<0> empty_static = "";
                PPR_ASSERT(empty_static.size() == 0);
                PPR_ASSERT(empty_static.view().empty());
            };
        }

        PPR_UNIT_TEST(literal) {
            _.recurse(Literal::static_interop);
            _.recurse(Literal::basic_view_and_equality);
            _.recurse(Literal::empty_string);
        };

        namespace Static {
            PPR_UNIT_TEST(basic_view_and_equality) {
                constexpr static_string a = "hello";
                constexpr static_string b = "hello";
                PPR_ASSERT(a == b);
                const auto sv = a.view();
                PPR_ASSERT(sv == "hello");
            };

            PPR_UNIT_TEST(copy_and_move) {
                constexpr static_string a = "hello";
                constexpr static_string b = a;
                PPR_ASSERT(a == b);
                constexpr static_string c = std::move(a);
                PPR_ASSERT(b == c);
            };

            PPR_UNIT_TEST(range) {
                constexpr static_string a = "hello";
                static_assert(std::ranges::contiguous_range<decltype(a)>);
                constexpr std::size_t n = std::ranges::size(a);
                PPR_ASSERT(n == 5);
            };

            PPR_UNIT_TEST(concatenation) {
                constexpr static_string a = "hello";
                constexpr static_string b = " world!";
                constexpr static static_string m = a + b;
                const auto sv = m.view();
                PPR_ASSERT(sv == "hello world!");
            };

            PPR_UNIT_TEST(edge_cases) {
                constexpr static_string a = "a";
                constexpr static_string b = "b";
                PPR_ASSERT(a <=> b == std::strong_ordering::less);
                PPR_ASSERT(b <=> a == std::strong_ordering::greater);

                constexpr static_string hello = "hello";
                constexpr static_string world = "world";
                constexpr auto combined = hello + world;
                PPR_ASSERT(combined.view() == "helloworld");
                static_assert(combined.size() == 10);
            };
        }

        PPR_UNIT_TEST(static_string) {
            _.recurse(Static::basic_view_and_equality);
            _.recurse(Static::copy_and_move);
            _.recurse(Static::range);
            _.recurse(Static::concatenation);
            _.recurse(Static::edge_cases);
        };

        namespace Range {
            PPR_UNIT_TEST(compare) {
                constexpr std::string_view a = "hello";
                constexpr std::string_view b = "HeLlO";
                PPR_ASSERT(a != b);
                PPR_ASSERT(caseFold(a) == caseFold(b));
                PPR_ASSERT((a <=> b) == ('h' <=> 'H'));
                PPR_ASSERT((caseFold(a) <=> caseFold(b)) == std::strong_ordering::equal);
            };

            PPR_UNIT_TEST(promote) {
                constexpr auto a = caseFold("_ToTo!");
                PPR_ASSERT(a == std::string_view("_ToTo!"));
                PPR_ASSERT(a == std::string_view("_toto!"));
                PPR_ASSERT(a == "_ToTo!");
                PPR_ASSERT(a == "_tOtO!");
                PPR_ASSERT(a != "!ToTo_");
                PPR_ASSERT((a <=> "_toto!") == std::strong_ordering::equal);
                PPR_ASSERT((a <=> "_tOTo!") == std::strong_ordering::equal);
                PPR_ASSERT((a <=> "_ZOTo!") == std::strong_ordering::less);
                PPR_ASSERT((a <=> "_aOTo!") == std::strong_ordering::greater);
            };

            PPR_UNIT_TEST(composition) {
                constexpr std::string_view a = "hello";
                constexpr std::string_view b = "HeLlO";
                PPR_ASSERT(a != b);
                PPR_ASSERT(toLower(a) == toLower(b));
                PPR_ASSERT((a <=> b) == ('h' <=> 'H'));
                PPR_ASSERT((toLower(a) <=> toLower(b)) == std::strong_ordering::equal);
            };

            PPR_UNIT_TEST(compositions_advanced) {
                constexpr std::string_view input = "  HeLlO   WoRlD!  ";

                auto squeezed = squeezeSpaces(trim(input));
                std::string intermediate(squeezed);

                auto processed = titleCase(intermediate);

                PPR_ASSERT(std::ranges::equal(processed, std::string_view("Hello World!")));

                PPR_ASSERT(std::ranges::equal(trim(caseFold("  HeLlO   ")), std::string_view("hello")));

                PPR_ASSERT(std::ranges::equal(toUpper("ABC"), std::string_view("ABC")));
            };

            PPR_UNIT_TEST(hashing) {
                constexpr std::string_view a = "hello";
                constexpr std::string_view b = "HeLlO";
                constexpr std::string_view c = " hello   ";

                const hash_t ha = hashValue(a);
                const hash_t hb = hashValue(b);
                const hash_t hc = hashValue(c);
                PPR_ASSERT(ha != hb);
                PPR_ASSERT(hb != hc);
                PPR_ASSERT(hc != ha);

                const hash_t hai = hashValue(caseFold(a));
                const hash_t hbi = hashValue(caseFold(b));
                const hash_t hci = hashValue(caseFold(c));
                PPR_ASSERT(hai == hbi);
                PPR_ASSERT(hbi != hci);
                PPR_ASSERT(hci != hai);
                PPR_ASSERT(ha != hai);
                PPR_ASSERT(hb != hbi);
                PPR_ASSERT(ha != hbi);

                const hash_t hat = hashValue(trim(a));
                const hash_t hbt = hashValue(trim(b));
                const hash_t hct = hashValue(trim(c));
                PPR_ASSERT(hat != hbt);
                PPR_ASSERT(hbt != hct);
                PPR_ASSERT(hct == hat);
                PPR_ASSERT(hat == hai);
                PPR_ASSERT(hct == hai);

                PPR_ASSERT(hashValue("long") != hashValue("longer"));
            };

            PPR_UNIT_TEST(to_string_conversion) {
                constexpr std::string_view sv = "Test123";
                std::string s1(toLower(sv));
                PPR_ASSERT(s1 == "test123");

                std::string s2(capitalize(sv));
                PPR_ASSERT(s2 == "Test123");

                std::string s3(trim("  hello  "));
                PPR_ASSERT(s3 == "hello");
            };
        }

        PPR_UNIT_TEST(range) {
            _.recurse(Range::compare);
            _.recurse(Range::promote);
            _.recurse(Range::composition);
            _.recurse(Range::compositions_advanced);
            _.recurse(Range::hashing);
            _.recurse(Range::to_string_conversion);
        };

        namespace Lazy {
            PPR_UNIT_TEST(trim_functions) {
                constexpr std::string_view s1 = "   hello   ";
                PPR_ASSERT(trim(s1) == "hello");

                constexpr std::string_view s2 = "\t\n\r hello world \f\v";
                PPR_ASSERT(trim(s2) == "hello world");

                constexpr std::string_view s3 = "no spaces";
                PPR_ASSERT(trim(s3) == "no spaces");

                constexpr std::string_view empty = "";
                PPR_ASSERT(trim(empty).empty());

                constexpr std::string_view only_spaces = "   \t\n  ";
                PPR_ASSERT(trim(only_spaces).empty());
            };

            PPR_UNIT_TEST(truncate_and_filter) {
                constexpr std::string_view s = "hello world";
                PPR_ASSERT(truncate(s, 5) == "hello");
                PPR_ASSERT(truncate(s, 100) == s);

                auto only_alpha = filterChars(s, &isAlpha<char>);
                PPR_ASSERT(only_alpha == "helloworld");
            };

            PPR_UNIT_TEST(squeeze_spaces) {
                constexpr std::string_view s1 = "hello   world";
                PPR_ASSERT(squeezeSpaces(s1) == "hello world");

                constexpr std::string_view s2 = "   multiple     \t\n   spaces   ";
                PPR_ASSERT(squeezeSpaces(s2) == " multiple spaces ");

                constexpr std::string_view s3 = "no extra spaces";
                PPR_ASSERT(squeezeSpaces(s3) == "no extra spaces");

                constexpr std::string_view s4 = " \t\n\r\f\v ";
                PPR_ASSERT(squeezeSpaces(s4) == " ");
            };

            PPR_UNIT_TEST(title_case) {
                constexpr std::string_view s1 = "hello world";
                PPR_ASSERT(titleCase(s1) == "Hello World");

                constexpr std::string_view s2 = "the quick brown fox";
                PPR_ASSERT(titleCase(s2) == "The Quick Brown Fox");

                constexpr std::string_view s3 = "  leading spaces";
                PPR_ASSERT(titleCase(s3) == "  Leading Spaces");

                constexpr std::string_view s4 = "mixed123CASE";
                PPR_ASSERT(titleCase(s4) == "Mixed123case");
            };

            PPR_UNIT_TEST(string_capitalize) {
                constexpr std::string_view s1 = "hello";
                PPR_ASSERT(capitalize(s1) == "Hello");

                constexpr std::string_view s2 = "hELLO";
                PPR_ASSERT(capitalize(s2) == "Hello");

                constexpr std::string_view s3 = "";
                PPR_ASSERT(capitalize(s3).empty());

                constexpr std::string_view s4 = "123abc";
                PPR_ASSERT(capitalize(s4) == "123abc");
            };

            PPR_UNIT_TEST(string_escape) {
                constexpr std::string_view s = R"(hello "world" \n)";
                auto escaped = stringEscape(s);
                const std::string result(escaped);
                PPR_ASSERT(result == R"(hello \"world\" \\n)");
            };

            PPR_UNIT_TEST(xml_escape) {
                constexpr std::string_view s = R"(<tag attr="value & 'more'>)";
                auto escaped = xmlEscape(s);
                const std::string result(escaped);
                PPR_ASSERT(result == "&lt;tag attr=&quot;value &amp; &apos;more&apos;&gt;&quot;");
            };

            PPR_UNIT_TEST(hex_encode) {
                constexpr std::string_view s = "\x01\xAB\xFF";
                auto hexed = hexEncode(s);
                const std::string result(hexed);
                PPR_ASSERT(result == "01abff");
            };
        }

        PPR_UNIT_TEST(lazy) {
            _.recurse(Lazy::trim_functions);
            _.recurse(Lazy::truncate_and_filter);
            _.recurse(Lazy::squeeze_spaces);
            _.recurse(Lazy::title_case);
            _.recurse(Lazy::string_capitalize);
            _.recurse(Lazy::string_escape);
            _.recurse(Lazy::xml_escape);
            _.recurse(Lazy::hex_encode);
        };
    }

    PPR_UNIT_TEST(strings) {
        _.recurse(Strings::helpers);
        _.recurse(Strings::literal);
        _.recurse(Strings::static_string);
        _.recurse(Strings::range);
        _.recurse(Strings::lazy);
    };
}
