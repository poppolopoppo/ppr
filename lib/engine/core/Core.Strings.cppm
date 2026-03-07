module;
#include "pP/Macros.h"

export module engine.core:strings;

import :assert;
import :containers;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // charset helper functions
    // ------------------------------------------------------------------

    template<details::TChar CharT>
    [[nodiscard]] constexpr CharT toLower(const CharT ch) noexcept {
        return (ch >= CharT('A') && ch <= CharT('Z')) ? ch + (CharT('a') - CharT('A')) : ch;
    }

    template<details::TChar CharT>
    [[nodiscard]] constexpr CharT toUpper(const CharT ch) noexcept {
        return (ch >= CharT('a') && ch <= CharT('z')) ? ch - (CharT('a') - CharT('A')) : ch;
    }

    template<details::TChar CharT>
    [[nodiscard]] constexpr bool isSpace(const CharT ch) noexcept {
        return ch == CharT(' ') || ch == CharT('\n') || ch == CharT('\r') ||
               ch == CharT('\t') || ch == CharT('\v') || ch == CharT('\f');
    }

    template<details::TChar CharT>
    [[nodiscard]] constexpr bool isDigit(const CharT ch) noexcept {
        return ch >= CharT('0') && ch <= CharT('9');
    }

    template<details::TChar CharT>
    [[nodiscard]] constexpr bool isAlpha(const CharT ch) noexcept {
        return (ch >= CharT('a') && ch <= CharT('z')) || (ch >= CharT('A') && ch <= CharT('Z'));
    }

    template<details::TChar CharT>
    [[nodiscard]] constexpr bool isAlnum(const CharT ch) noexcept {
        return isAlpha(ch) || isDigit(ch);
    }

    // ------------------------------------------------------------------
    // a string literal has no lifetime and can only be constructed from a static string
    // ------------------------------------------------------------------

    template<details::TChar CharT>
    class [[nodiscard]] basic_string_literal {
        const CharT *m_str{nullptr};

    public:
        template<std::size_t nLen> requires (nLen > 0u)
        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr basic_string_literal(const CharT (&str)[nLen]) noexcept
            : m_str{str} {
        }

        template<std::size_t nLen> requires (nLen > 0u)
        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr basic_string_literal(const std::array<CharT, nLen> &arr) noexcept
            : m_str{arr.data()} {
            PPR_ASSERT(arr.back() == CharT{});
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return std::char_traits<CharT>::length(m_str);
        }

        using view_type = std::basic_string_view<CharT>;
        using value_type = CharT;
        using iterator = std::ranges::iterator_t<view_type>;

        [[nodiscard]] constexpr iterator begin() const noexcept {
            return view().begin();
        }

        [[nodiscard]] constexpr iterator end() const noexcept {
            return view().end();
        }

        [[nodiscard]] constexpr std::basic_string_view<CharT> view() const noexcept {
            return {m_str};
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator std::basic_string_view<CharT>() const noexcept {
            return {m_str};
        }

        [[nodiscard]] friend constexpr bool operator==(const basic_string_literal &lhs, const basic_string_literal &rhs) noexcept {
            return lhs.view() == rhs.view();
        }

        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const basic_string_literal &lhs, const basic_string_literal &rhs) noexcept {
            return lhs.view() <=> rhs.view();
        }
    };

    using string_literal = basic_string_literal<char>;
    using wstring_literal = basic_string_literal<wchar_t>;
    using u8string_literal = basic_string_literal<char8_t>;

    namespace hal::native {
        using string_literal = basic_string_literal<char_t>;
    }

    template<details::TChar CharT>
    struct details::relocatable<basic_string_literal<CharT> > : std::true_type {
    };

    // ------------------------------------------------------------------
    // static strings are immutable and constexpr
    // ------------------------------------------------------------------

    template<details::TChar CharT, std::size_t nLen>
    struct [[nodiscard]] basic_static_string {
        std::array<CharT, nLen + 1u/* \0 */> m_chars{};

        constexpr basic_static_string() noexcept = default;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr basic_static_string(const CharT (&str)[nLen + 1u/* \0 */]) noexcept {
            std::copy(std::begin(str), std::end(str), m_chars.begin());
            PPR_ASSERT(m_chars.back() == CharT{});
        }

        [[nodiscard]] constexpr auto &operator[](this auto &&self, const std::size_t index) noexcept {
            return self.m_chars[index];
        }

        using value_type = CharT;
        using iterator = decltype(std::begin(m_chars));

        [[nodiscard]] constexpr auto begin(this auto &&self) noexcept {
            return std::begin(self.m_chars);
        }

        [[nodiscard]] constexpr auto end(this auto &&self) noexcept {
            return std::begin(self.m_chars) + nLen; /* remove final \0 */
        }

        [[nodiscard]] constexpr const CharT *data() const noexcept {
            return m_chars.data();
        }

        [[nodiscard]] constexpr const CharT *c_str() const noexcept {
            return m_chars.data();
        }

        [[nodiscard]] static constexpr std::size_t size() noexcept {
            return nLen;
        }

        [[nodiscard]] constexpr basic_string_literal<CharT>
        literal() const noexcept {
            return {m_chars};
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator basic_string_literal<CharT>() const noexcept {
            return literal();
        }

        [[nodiscard]] constexpr std::basic_string_view<CharT> view() const noexcept {
            return {m_chars.data(), nLen};
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator std::basic_string_view<CharT>() const noexcept {
            return view();
        }

        template<std::size_t nOffset, std::size_t nNewLen> requires (nOffset + nNewLen <= nLen)
        [[nodiscard]] constexpr basic_static_string<CharT, nNewLen>
        subrange() const noexcept {
            basic_static_string<CharT, nNewLen> outp;
            const auto first = m_chars.begin() + nOffset;
            std::copy(first, first + nNewLen, outp.m_chars.begin());
            PPR_ASSERT(outp.m_chars.back() == CharT{});
            return outp;
        }

        template<std::size_t nOtherLen>
        [[nodiscard]] friend constexpr auto
        operator +(const basic_static_string &lhs, const basic_static_string<CharT, nOtherLen> &rhs) noexcept {
            basic_static_string<CharT, nLen + nOtherLen> result;
            auto outp = std::copy(lhs.begin(), lhs.end(), result.begin());
            std::copy(rhs.begin(), rhs.end(), outp);
            return result;
        }

        [[nodiscard]] friend constexpr bool
        operator ==(const basic_static_string &lhs, const basic_static_string &rhs) noexcept {
            return std::ranges::equal(lhs.m_chars, rhs.m_chars);
        }

        template<std::size_t nOtherLen>
        [[nodiscard]] friend constexpr bool
        operator ==(const basic_static_string &, const basic_static_string<CharT, nOtherLen> &) noexcept {
            return false;
        }

        template<std::size_t nOtherLen>
        [[nodiscard]] friend constexpr std::strong_ordering
        operator <=>(const basic_static_string &lhs, const basic_static_string<CharT, nOtherLen> &rhs) noexcept {
            return std::lexicographical_compare_three_way(
                std::begin(lhs), std::end(lhs),
                std::begin(rhs), std::end(rhs),
                std::compare_three_way{});
        }
    };

    template<std::size_t nLen>
    using static_string = basic_static_string<char, nLen>;
    template<std::size_t nLen>
    using wstatic_string = basic_static_string<wchar_t, nLen>;
    template<std::size_t nLen>
    using u8static_string = basic_static_string<char8_t, nLen>;

    namespace hal::native {
        template<std::size_t nLen>
        using static_string = basic_static_string<char_t, nLen>;
    }

    template<details::TChar CharT, std::size_t nLenPlus1> requires (nLenPlus1 > 0u)
    basic_static_string(const CharT (&str)[nLenPlus1]) -> basic_static_string<CharT, nLenPlus1 - 1u>;

    template<details::TChar CharT, std::size_t nLen>
    struct details::relocatable<basic_static_string<CharT, nLen> > : std::true_type {
    };

    // ------------------------------------------------------------------
    // character escaping
    // ------------------------------------------------------------------

    // a small, stack-owned character buffer returned by value.
    template<details::TChar CharT, std::size_t nCapacity>
    struct [[nodiscard]] basic_string_glyph {
        std::array<CharT, nCapacity> m_buf{};
        std::size_t m_len{0};

        // Construct from a string literal — used by all named escape arms.
        // The array is copied into m_buf at construction time; no pointer escapes.
        template<std::size_t N> requires (N - 1u <= nCapacity)
        constexpr basic_string_glyph(const CharT (&lit)[N]) noexcept : m_len{N - 1u} {
            std::copy_n(lit, N - 1u, m_buf.begin());
        }

        // Construct from a single character — used by the passthrough default arm.
        // Stores the value, never the address of the caller's variable.
        constexpr basic_string_glyph(CharT ch) noexcept : m_len{1} { // NOLINT(*-explicit-*)
            m_buf[0] = ch;
        }

        // Contiguous range interface — satisfies views::join's requirements.
        [[nodiscard]] constexpr const CharT *begin() const noexcept { return m_buf.data(); }
        [[nodiscard]] constexpr const CharT *end() const noexcept { return m_buf.data() + m_len; }
        [[nodiscard]] constexpr std::size_t size() const noexcept { return m_len; }

        [[nodiscard]] constexpr std::basic_string_view<CharT> view() const noexcept {
            return {m_buf.data(), m_len};
        }

        [[nodiscard]] constexpr operator std::basic_string_view<CharT>() const noexcept {
            return {m_buf.data(), m_len};
        }
    };

    template<details::TChar CharT>
    [[nodiscard]] constexpr basic_string_glyph<CharT, 2u> charEscape(const CharT &ch) noexcept {
        switch (ch) {
            case PPR_LITERAL_FOR(CharT, '\n'): return PPR_LITERAL_FOR(CharT, "\\n");
            case PPR_LITERAL_FOR(CharT, '\r'): return PPR_LITERAL_FOR(CharT, "\\r");
            case PPR_LITERAL_FOR(CharT, '\t'): return PPR_LITERAL_FOR(CharT, "\\t");
            case PPR_LITERAL_FOR(CharT, '\0'): return PPR_LITERAL_FOR(CharT, "\\0");
            case PPR_LITERAL_FOR(CharT, '\\'): return PPR_LITERAL_FOR(CharT, "\\\\");
            case PPR_LITERAL_FOR(CharT, '"'): return PPR_LITERAL_FOR(CharT, "\\\"");
            case PPR_LITERAL_FOR(CharT, '\''): return PPR_LITERAL_FOR(CharT, "\\'");
            default: return {ch};
        }
    }

    template<details::TChar CharT>
    [[nodiscard]] constexpr basic_string_glyph<CharT, 6u> xmlEscapeChar(const CharT &ch) noexcept {
        switch (ch) {
            case PPR_LITERAL_FOR(CharT, '&'): return PPR_LITERAL_FOR(CharT, "&amp;");
            case PPR_LITERAL_FOR(CharT, '<'): return PPR_LITERAL_FOR(CharT, "&lt;");
            case PPR_LITERAL_FOR(CharT, '>'): return PPR_LITERAL_FOR(CharT, "&gt;");
            case PPR_LITERAL_FOR(CharT, '"'): return PPR_LITERAL_FOR(CharT, "&quot;");
            case PPR_LITERAL_FOR(CharT, '\''): return PPR_LITERAL_FOR(CharT, "&apos;");
            default: return ch;
        }
    }

    // ------------------------------------------------------------------
    // case insensitive character adapter
    // ------------------------------------------------------------------

    template<details::TChar CharT>
    struct [[nodiscard]] CaseFoldChar {
        CharT m_char{};

        constexpr CaseFoldChar() noexcept = default;

        explicit constexpr CaseFoldChar(const CharT ch) noexcept
            : m_char(ch) {
        }

        [[nodiscard]] constexpr CharT operator*() const noexcept {
            return toLower(m_char);
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator CharT() const noexcept {
            return toLower(m_char);
        }

        [[nodiscard]] friend constexpr bool
        operator==(const CaseFoldChar lhs, const CaseFoldChar rhs) noexcept {
            return *lhs == *rhs;
        }

        [[nodiscard]] friend constexpr std::strong_ordering
        operator<=>(const CaseFoldChar lhs, const CaseFoldChar rhs) noexcept {
            return *lhs <=> *rhs;
        }

        [[nodiscard]] friend constexpr bool
        operator==(const CaseFoldChar lhs, const CharT rhs) noexcept {
            return *lhs == toLower(rhs);
        }

        [[nodiscard]] friend constexpr std::strong_ordering
        operator<=>(const CaseFoldChar lhs, const CharT rhs) noexcept {
            return *lhs <=> toLower(rhs);
        }

        [[nodiscard]] friend constexpr hash_t
        hashValue(const CaseFoldChar ch) noexcept {
            return hashValue(*ch);
        }
    };

    template<details::TChar CharT>
    CaseFoldChar(CharT ch) -> CaseFoldChar<CharT>;

    template<details::TChar CharT>
    constexpr CaseFoldChar<CharT> caseFold(const CharT ch) noexcept {
        return CaseFoldChar<CharT>(ch);
    }

    using ichar = CaseFoldChar<char>;
    using iwchar_t = CaseFoldChar<wchar_t>;
    using ichar8_t = CaseFoldChar<char8_t>;

    // report CaseFoldChar<> as relocatable
    template<details::TChar CharT>
    struct details::relocatable<CaseFoldChar<CharT> > : std::true_type {
    };

    // report CaseFoldChar<> as a valid string character type
    template<details::TChar CharT>
    struct details::string_character<CaseFoldChar<CharT> > : std::true_type {
    };

    static_assert(std::equality_comparable_with<char, char>);
    static_assert(std::three_way_comparable_with<char, char>);

    static_assert(std::equality_comparable_with<CaseFoldChar<char>, char>);
    static_assert(std::equality_comparable_with<char, CaseFoldChar<char> >);

    static_assert(std::three_way_comparable_with<CaseFoldChar<char>, char>);
    static_assert(std::three_way_comparable_with<char, CaseFoldChar<char> >);

    // ------------------------------------------------------------------
    // string range for in-memory transforms
    // ------------------------------------------------------------------

    namespace details {
        template<typename CharactersT>
        concept TCharRange = std::ranges::viewable_range<CharactersT> &&
                             TChar<std::ranges::range_value_t<CharactersT> >;
    }

    template<details::TCharRange CharactersT>
    class [[nodiscard]] basic_string_range
            : public std::ranges::view_interface<basic_string_range<CharactersT> > {
        static_assert(
            std::is_same_v<std::remove_cvref_t<CharactersT>, CharactersT>);

        CharactersT m_view;

    public:
        static constexpr bool enable_view = true;
        using value_type = std::ranges::range_value_t<CharactersT>;
        using iterator = std::ranges::iterator_t<CharactersT>;

        constexpr basic_string_range(CharactersT &&view) noexcept
            : m_view(std::forward<CharactersT>(view)) {
        }

        constexpr basic_string_range(const CharactersT &view) noexcept
            : m_view(view) {
        }

        [[nodiscard]] constexpr auto begin() noexcept {
            return std::ranges::begin(m_view);
        }

        [[nodiscard]] constexpr auto begin() const noexcept
            requires std::ranges::range<const CharactersT> {
            return std::ranges::begin(m_view);
        }

        [[nodiscard]] constexpr auto end() noexcept {
            return std::ranges::end(m_view);
        }

        [[nodiscard]] constexpr auto end() const noexcept
            requires std::ranges::range<const CharactersT> {
            return std::ranges::end(m_view);
        }

        [[nodiscard]] constexpr std::size_t size() noexcept
            requires std::ranges::sized_range<CharactersT> {
            return std::ranges::size(m_view);
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept
            requires std::ranges::sized_range<const CharactersT> {
            return std::ranges::size(m_view);
        }

        [[nodiscard]] constexpr auto view(this auto &&self) noexcept {
            return std::ranges::subrange(self.begin(), self.end());
        }

        template<typename CharTraitsT, typename AllocatorT>
        [[nodiscard]] explicit
        operator std::basic_string<value_type, CharTraitsT, AllocatorT>() {
            return std::basic_string<value_type, CharTraitsT, AllocatorT>(std::from_range, *this);
        }

        template<typename CharTraitsT, typename AllocatorT>
        [[nodiscard]] explicit
        operator std::basic_string<value_type, CharTraitsT, AllocatorT>() const
            requires std::ranges::range<const CharactersT> {
            return std::basic_string<value_type, CharTraitsT, AllocatorT>(std::from_range, *this);
        }

        [[nodiscard]] friend constexpr hash_t hashValue(basic_string_range value) noexcept {
            return hash::anyRange(value);
        }

        template<std::ranges::range RhsT>
            requires std::equality_comparable_with<value_type, std::ranges::range_value_t<RhsT> >
        [[nodiscard]] friend constexpr bool operator==(
            basic_string_range lhs, RhsT &&rhs) noexcept {
            return std::ranges::equal(lhs, std::forward<RhsT>(rhs));
        }

        template<std::ranges::range RhsT>
            requires std::three_way_comparable_with<value_type, std::ranges::range_value_t<RhsT> >
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(
            basic_string_range lhs, RhsT &&rhs) noexcept {
            return std::lexicographical_compare_three_way(
                lhs.begin(), lhs.end(),
                std::ranges::begin(rhs), std::ranges::end(rhs));
        }

        template<details::TChar CharT, std::size_t N>
            requires std::equality_comparable_with<value_type, CharT>
        [[nodiscard]] friend constexpr bool operator==(
            basic_string_range lhs, const CharT (&rhs)[N]) noexcept {
            auto rhs_view = std::basic_string_view<CharT>(rhs);
            return std::ranges::equal(lhs, rhs_view);
        }

        template<details::TChar CharT, std::size_t N>
            requires std::three_way_comparable_with<value_type, CharT>
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(
            basic_string_range lhs, const CharT (&rhs)[N]) noexcept {
            auto rhs_view = std::basic_string_view<CharT>(rhs);
            return std::lexicographical_compare_three_way(
                lhs.begin(), lhs.end(),
                rhs_view.begin(), rhs_view.end());
        }
    };

    template<details::TCharRange CharactersT>
    basic_string_range(CharactersT &&) -> basic_string_range<std::remove_cvref_t<CharactersT> >;

    template<details::TChar CharT, std::size_t N>
    basic_string_range(const CharT (&)[N])
        -> basic_string_range<std::basic_string_view<CharT> >;

    template<details::TCharRange CharactersT>
    basic_string_range(basic_string_range<CharactersT> &&)
        -> basic_string_range<CharactersT>;

    template<details::TCharRange V>
        requires std::is_same_v<std::ranges::range_value_t<V>, char>
    using string_range = basic_string_range<V>;
    template<details::TCharRange V>
        requires std::is_same_v<std::ranges::range_value_t<V>, wchar_t>
    using wstring_range = basic_string_range<V>;
    template<details::TCharRange V>
        requires std::is_same_v<std::ranges::range_value_t<V>, char8_t>
    using u8string_range = basic_string_range<V>;

    // ---------------------------------------------------------------------------
    // lazy in-memory string transformation helpers
    // ---------------------------------------------------------------------------

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto caseFold(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(std::views::transform(
            basic_string_range(std::forward<CharactersT>(characters)),
            &caseFold<char_type>));
    }

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto toLower(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(std::views::transform(
            basic_string_range(std::forward<CharactersT>(characters)),
            &toLower<char_type>));
    }

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto toUpper(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(std::views::transform(
            basic_string_range(std::forward<CharactersT>(characters)),
            &toUpper<char_type>));
    }

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto capitalize(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(
            pP::views::enumerate(std::forward<CharactersT>(characters)) |
            std::views::transform([](auto p) constexpr -> char_type {
                auto [i, c] = p;
                return i == 0 ? toUpper(c) : toLower(c);
            }));
    }

    template<details::TCharRange CharactersT> requires std::ranges::contiguous_range<CharactersT>
    [[nodiscard]] auto stringEscape(CharactersT &&characters) noexcept {
        // Takes const char& so the string_view{&c, 1} points into the original
        // buffer. Safe because contiguous_range guarantees stable element
        // addresses.
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::transform(&charEscape<char_type>) | std::views::join);
    }

    template<details::TCharRange CharactersT, typename PredicateT>
        requires std::predicate<PredicateT, std::ranges::range_value_t<CharactersT> >
    [[nodiscard]] constexpr auto filterChars(CharactersT &&characters,
                                             PredicateT &&pred) noexcept {
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::filter(std::forward<PredicateT>(pred)));
    }

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto trimLeft(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::drop_while(&isSpace<char_type>));
    }

    // Requires bidirectional range for the double-reverse trick
    template<details::TCharRange CharactersT> requires std::ranges::bidirectional_range<CharactersT>
    [[nodiscard]] constexpr auto trimRight(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::reverse | std::views::drop_while(&isSpace<char_type>) |
            std::views::reverse);
    }

    template<details::TCharRange CharactersT> requires std::ranges::bidirectional_range<CharactersT>
    [[nodiscard]] constexpr auto trim(CharactersT &&characters) noexcept {
        // Can't compose trimLeft(trimRight(...)) directly — trimRight returns a
        // basic_string_range wrapping a reverse_view, which is bidirectional,
        // so trimLeft's drop_while still works on it.
        return trimLeft(trimRight(std::forward<CharactersT>(characters)));
    }

    // views::take is the simplest possible range operation
    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto truncate(CharactersT &&characters,
                                          std::size_t maxLen) noexcept {
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::take(maxLen));
    }

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto squeezeSpaces(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        // Each chunk is either an all-space run (emit single ' ') or
        // a non-space run (emit as-is as a string_view).
        // Both arms must return the same type — string_view<char_type>.
        // For non-space runs this requires the input to be contiguous.
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::chunk_by(
                [](char_type a, char_type b) { return isSpace(a) == isSpace(b); }) |
            std::views::transform([](auto &&chunk) -> auto {
                if (isSpace(*chunk.begin()))
                    return std::ranges::subrange(
                        chunk.begin(),
                        chunk.begin() + 1); // collapse entire space run
                return std::ranges::subrange(chunk.begin(), chunk.end());
            }) |
            std::views::join);
    }

    template<details::TCharRange CharactersT>
    [[nodiscard]] constexpr auto titleCase(CharactersT &&characters) {
        using char_type = std::ranges::range_value_t<CharactersT>;
        // Chunk into alternating word/non-word runs, then capitalize the first
        // character of each word chunk via views::enumerate inside
        // views::transform.
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::chunk_by(
                [](char_type a, char_type b) { return isAlnum(a) == isAlnum(b); }) |
            std::views::transform([](auto &&chunk) {
                return pP::views::enumerate(chunk) |
                       std::views::transform([](auto p) -> auto {
                           auto [i, c] = p;
                           return i == 0 && isAlpha(c) ? toUpper(c) : toLower(c);
                       });
            }) |
            std::views::join);
    }

    template<details::TCharRange CharactersT> requires std::ranges::contiguous_range<CharactersT>
    [[nodiscard]] constexpr auto xmlEscape(CharactersT &&characters) noexcept {
        using char_type = std::ranges::range_value_t<CharactersT>;
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::transform(&xmlEscapeChar<char_type>) | std::views::join);
    }

    template<details::TCharRange CharactersT>
        requires std::ranges::contiguous_range<CharactersT> &&
                 std::is_same_v<std::ranges::range_value_t<CharactersT>, char>
    [[nodiscard]] constexpr auto hexEncode(CharactersT &&characters) noexcept {
        // Each byte expands to exactly 2 hex characters stored in a thread_local
        // scratch buffer — this is the unavoidable cost of 1→2 expansion without
        // allocation when the output can't be a string_view into the input.
        return basic_string_range(
            basic_string_range(std::forward<CharactersT>(characters)) |
            std::views::transform([](const char c) noexcept -> std::array<char, 2> {
                const auto b = static_cast<unsigned char>(c);
                constexpr std::string_view table = "0123456789abcdef";
                return {table[b >> 4], table[b & 0xF]};
            }) |
            std::views::join // join over arrays works since array satisfies range
        );
    }
}

// ------------------------------------------------------------------
// std::formatter<> specialization for CaseFoldView
// ------------------------------------------------------------------

export namespace std {
    template<pP::details::TChar CharT>
    struct formatter<pP::CaseFoldChar<CharT>, CharT>
            : formatter<CharT, CharT> {
        constexpr void set_debug_format() const noexcept {
        }

        template<typename FormatContext>
        constexpr auto format(const pP::CaseFoldChar<CharT> &c,
                              FormatContext &ctx) const {
            return formatter<CharT, CharT>::format(static_cast<CharT>(c), ctx);
        }
    };

    template<pP::details::TCharRange CharactersT, pP::details::TChar CharT>
    struct formatter<pP::basic_string_range<CharactersT>, CharT> {
        using char_type = pP::basic_string_range<CharactersT>::value_type;

        constexpr auto parse(std::basic_format_parse_context<CharT> &ctx) {
            return ctx.begin();
        }

        template<typename FormatContext>
        auto format(const pP::basic_string_range<CharactersT> &range,
                    FormatContext &ctx) const {
            if constexpr (std::is_same_v<CharT, char_type>) {
                return std::copy(range.begin(), range.end(), ctx.out());
            } else {
                auto dst = ctx.out();
                for (auto src = range.begin(); src != range.end(); ++src, ++dst) {
                    *dst = static_cast<CharT>(*src);
                }
                return dst;
            }
        }
    };
}
