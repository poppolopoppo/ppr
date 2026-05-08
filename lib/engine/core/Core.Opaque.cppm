module;
#include "pP/Macros.h"

export module engine.core:opaque;

import :assert;
import :function_ref;
import :strings;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // opaque::Value represents arbitrary structured data without a
    // predefined schema, principally for structured logging:
    //
    //   log("a message", opaque::Dict{
    //       {"width",      window->m_width},
    //       {"height",     window->m_height},
    //       {"has_active", window->m_has_focus},
    //   });
    //
    // Lifetime note: Array and Dict are non-owning spans. Their backing
    // storage must outlive the Value that wraps them. Brace-init at a
    // call site (as above) is safe because the initializer_list lives
    // for the entire full-expression. Do not store a Dict/Array Value
    // across statement boundaries without ensuring the backing data
    // remains live.
    // ------------------------------------------------------------------

    namespace opaque {
        struct Value;

        // --------------------------------------------------------------
        // type erasure helper for text formatting
        // --------------------------------------------------------------

        template<details::TChar CharT>
        struct [[nodiscard]] basic_format_context {
            virtual void write(std::basic_string_view<CharT> chunk) = 0;

            struct iterator {
                using difference_type = std::ptrdiff_t;

                basic_format_context *m_sink{nullptr};

                iterator &operator=(char c) {
                    m_sink->write({&c, 1});
                    return *this;
                }

                iterator &operator*() { return *this; }
                iterator &operator++() { return *this; }
                iterator operator++(int) { return *this; }
            };

            [[nodiscard]] constexpr iterator out() noexcept {
                return {this};
            }

        protected:
            ~basic_format_context() = default;
        };

        using format_context = basic_format_context<char>;
        static_assert(std::output_iterator<format_context::iterator, char>);

        // --------------------------------------------------------------
        // opaque value variants
        // --------------------------------------------------------------

        using KeyValue = std::pair<string_literal, Value>;

        using String = std::string_view;
        using WString = std::wstring_view;
        using U8String = std::u8string_view;
        using Array = array_view<Value>;
        using Dict = array_view<KeyValue>;
        using Fn = std23::function_ref<Value()>;
        using Formatter = std23::function_ref<void(format_context &)>;

        namespace details {
            // All alias types (string_view, span, function_ref) are already
            // lightweight non-owning handles, so we store them directly.
            using ValueVariant =
            std::variant<
                bool,

                char, wchar_t, char8_t,

                i8, i16, i32, i64,

                u8, u16, u32, u64,

                float, double,

                std::string_view, std::wstring_view, std::u8string_view,

                Array, Dict, Fn, Formatter>;
        } // namespace details

        struct [[nodiscard]] Value : details::ValueVariant {
            using super_t = details::ValueVariant;
            using super_t::super_t;
            using super_t::operator=;

            // ReSharper disable once CppNonExplicitConvertingConstructor
            constexpr Value(std::initializer_list<Value> arr PPR_LIFETIME_BOUND) noexcept;

            // ReSharper disable once CppNonExplicitConvertingConstructor
            constexpr Value(std::initializer_list<KeyValue> dict PPR_LIFETIME_BOUND) noexcept;

            // Allow direct initialization from functors convertible to Fn
            template<typename FunctorT>
                requires (!std::is_same_v<std::decay_t<FunctorT>, Fn> &&
                          requires(FunctorT &&f) { Fn{std::forward<FunctorT>(f)}; })
            // ReSharper disable once CppNonExplicitConvertingConstructor
            constexpr Value(FunctorT &&functor) noexcept
                : super_t(Fn{std::forward<FunctorT>(functor)}) {
            }

            // Allow direct initialization from functors convertible to Formatter
            template<typename FunctorT>
                requires (!std::is_same_v<std::decay_t<FunctorT>, Formatter> &&
                          requires(FunctorT &&f) { Formatter{std::forward<FunctorT>(f)}; })
            // ReSharper disable once CppNonExplicitConvertingConstructor
            constexpr Value(FunctorT &&functor) noexcept
                : super_t(Formatter{std::forward<FunctorT>(functor)}) {
            }

            // Note: operator== is intentionally not provided. std::variant::operator==
            // requires all alternatives to be equality_comparable, but Array/Dict
            // have no operator==, and Fn/Formatter (function_ref) have no
            // meaningful identity.

            // Returns a reference to the held value. Undefined behavior if the
            // active alternative is not T — only call when the type is certain.
            template<typename T>
            [[nodiscard]] constexpr const T &get() const noexcept {
                return std::get<T>(*this);
            }

            // Returns a pointer to the held value, or nullptr if the active
            // alternative is not T.
            template<typename T>
            [[nodiscard]] constexpr const T *as() const noexcept {
                return std::get_if<T>(this);
            }
        };

        constexpr Value::Value(const std::initializer_list<Value> arr) noexcept
            : details::ValueVariant(Array(arr)) {
        }

        constexpr Value::Value(const std::initializer_list<KeyValue> dict) noexcept
            : details::ValueVariant(Dict(dict)) {
        }
    }

    // --------------------------------------------------------------
    // allocator can relocate opaque::Value
    // --------------------------------------------------------------

    template<details::TChar CharT>
    struct details::relocatable<opaque::basic_format_context<CharT> > : std::true_type {
    };

    template<>
    struct details::relocatable<opaque::Value> : std::true_type {
    };

    template<>
    struct details::relocatable<opaque::KeyValue> : std::true_type {
    };
}

// --------------------------------------------------------------
// opaque value formatting
// --------------------------------------------------------------

export namespace std {
    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Fn, CharT>
            : formatter<pP::opaque::Value, CharT> {
        using super_t = formatter<pP::opaque::Value, CharT>;

        template<typename FormatContextT>
        auto format(const pP::opaque::Fn &fn, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            return super_t::format(fn(), ctx);
        }
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Formatter, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::opaque::Formatter &fmt, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            // Concrete sink wrapping whatever output iterator this context uses.
            // Stack-allocated — zero heap overhead.
            struct format_sink final : pP::opaque::format_context {
                FormatContextT &ctx;

                explicit format_sink(FormatContextT &c) : ctx(c) {
                }

                void write(string_view sv) override {
                    ctx.advance_to(ranges::copy(sv, ctx.out()).out);
                }
            };
            format_sink sink{ctx};
            fmt(sink);
            return ctx.out();
        }
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Value, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::opaque::Value &value, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            return visit(
                pP::overloaded(
                    [&]<pP::details::TChar StringCharT>(const basic_string_view<StringCharT> &inner_value) {
                        return format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:?}"),
                                         inner_value);
                    },
                    [&]<typename ValueT>(const ValueT &inner_value)
                        requires formattable<ValueT, CharT> {
                        return format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:}"),
                                         inner_value);
                    },
                    [&]<typename UnformattableT>([[maybe_unused]] const UnformattableT &) noexcept {
                        // Fallback for cross-width types
                        return format_to(ctx.out(),
                                         PPR_LITERAL_FOR(CharT, "fallback <{}> ?"),
                                         typeid(UnformattableT).name());
                    }),
                value);
        }
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::KeyValue, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::opaque::KeyValue &pair, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            return format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:?}: {:}"),
                             pair.first.view(), pair.second);
        }

        static constexpr void set_brackets(basic_string_view<CharT>, basic_string_view<CharT>) noexcept {}
        static constexpr void set_separator(basic_string_view<CharT>) noexcept {}
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Dict, CharT>
            : range_formatter<pP::opaque::KeyValue, CharT> {
        using super_t = range_formatter<pP::opaque::KeyValue, CharT>;

        constexpr formatter() noexcept {
            constexpr std::basic_string_view<CharT> bracket_open = PPR_LITERAL_FOR(CharT, "{");
            constexpr std::basic_string_view<CharT> bracket_close = PPR_LITERAL_FOR(CharT, "}");
            super_t::set_brackets(bracket_open, bracket_close);
        }
    };
}
