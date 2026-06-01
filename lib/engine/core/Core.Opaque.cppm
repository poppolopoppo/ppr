module;
#include "pP/Macros.h"

export module engine.core:opaque;

import :assert;
import :allocator;
import :function_ref;
import :strings;

import std;

export namespace pP {
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
        using Formatter = std23::function_ref<void(format_context &)>;

        namespace details {
            // All alias types (string_view, span, function_ref) are already
            // lightweight non-owning handles, so we store them directly.
            using ValueVariant = std::variant<
                bool,

                char, wchar_t, char8_t,

                i8, i16, i32, i64,

                u8, u16, u32, u64,

                float, double,

                String, WString, U8String,

                Array,
                Dict,

                Formatter>;

            static_assert(sizeof(ValueVariant) == sizeof(void *) * 3u);
        } // namespace details

        struct [[nodiscard]] Value : details::ValueVariant {
            using super_t = details::ValueVariant;
            using super_t::super_t;
            using super_t::operator=;

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

        // --------------------------------------------------------------
        // Value are transient, Block is persistent
        // --------------------------------------------------------------

        struct Block {
            struct Value;

            using String = relative_view<char>;
            using WString = relative_view<wchar_t>;
            using U8String = relative_view<char8_t>;
            using Array = relative_view<Value>;
            using KeyValue = std::pair<String, Value>;

            struct Dict : relative_view<KeyValue> {
                using relative_view::relative_view;
                using relative_view::operator=;
                using relative_view::operator[];

                [[nodiscard]] constexpr const Value *
                tryGet(const string_literal key) const noexcept {
                    for (const auto &[first, second]: *this) {
                        if (std::string_view(first.data(), first.size()) == key) {
                            return std::addressof(second);
                        }
                    }
                    return nullptr;
                }

                [[nodiscard]] constexpr const Value &
                get(const string_literal key) const noexcept {
                    if (const Value *const p_value = tryGet(key)) [[likely]] {
                        return *p_value;
                    }
                    std::unreachable();
                }

                [[nodiscard]] constexpr const Value &
                operator[](const string_literal key) const noexcept {
                    return get(key);
                }
            };

            using ValueVariant = std::variant<
                bool,

                char, wchar_t, char8_t,

                i8, i16, i32, i64,

                u8, u16, u32, u64,

                float, double,

                String, WString, U8String,

                Array,
                Dict>;

            struct Value : ValueVariant {
                using super_t = ValueVariant;
                using super_t::super_t;

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

            static_assert(sizeof(Value) == sizeof(void *) * 2u);

            template<mem::details::TArenaAllocator ArenaT>
            struct Builder {
                Value &m_target;
                ArenaT &m_arena;

                constexpr Builder(Value &target, ArenaT &arena) noexcept
                    : m_target(target), m_arena(arena) {
                }

                constexpr void dup(const opaque::Value &init) const noexcept {
                    std::visit(*this, init);
                }

                template<typename NumericT>
                    requires std::integral<NumericT> || std::floating_point<NumericT>
                void operator()(const NumericT numeric) const noexcept {
                    m_target.emplace<NumericT>(numeric);
                }

                template<pP::details::TChar CharT>
                void operator()(const std::basic_string_view<CharT> str) const noexcept {
                    const std::span<CharT> local_cpy = m_arena.dup(str);
                    m_target.emplace<relative_view<CharT> >(local_cpy);
                }

                void operator()(const opaque::Array arr) const noexcept {
                    const auto dst = m_arena.template span<Value>(arr.size());
                    m_target.emplace<Array>(dst);

                    auto output = dst.begin();
                    for (const opaque::Value &src: arr) {
                        Value &it = *std::construct_at(std::addressof(*output++));
                        Builder{it, m_arena}.dup(src);
                    }
                }

                void operator()(const opaque::Dict dict) const noexcept {
                    const auto dst = m_arena.template span<KeyValue>(dict.size());
                    m_target.emplace<Dict>(dst);

                    auto output = dst.begin();
                    for (const auto &[key, value]: dict) {
                        KeyValue &it = *std::construct_at(std::addressof(*output++));
                        it.first = m_arena.dup(key);
                        Builder{it.second, m_arena}.dup(value);
                    }
                }

                void operator()(const Formatter fmt) const noexcept {
                    // Concrete sink wrapping whatever output iterator this context uses.
                    // Stack-allocated — zero heap overhead.
                    struct format_sink final : format_context {
                        ArenaT &m_arena;
                        mem::Allocation<char, ArenaT> m_output;

                        explicit constexpr format_sink(ArenaT &arena PPR_LIFETIME_BOUND) noexcept
                            : m_arena(arena) {
                        }

                        void write(const std::string_view sv) override {
                            const std::size_t off = m_output.count();
                            if (PPR_ENSURE(m_output.resize(m_arena, m_output.count() + sv.size()))) [[likely]] {
                                std::memcpy(m_output.data() + off, sv.data(), sv.size() * sizeof(sv[0]));
                            }
                        }
                    };

                    format_sink sink{m_arena};
                    fmt(sink);

                    m_target.emplace<String>(sink.m_output.discard());
                }
            };

            Dict *m_data{nullptr};

            [[nodiscard]] constexpr bool empty() const noexcept {
                return m_data ? m_data->empty() : true;
            }

            [[nodiscard]] constexpr std::size_t size() const noexcept {
                return m_data ? m_data->size() : 0u;
            }

            [[nodiscard]] constexpr const Dict *operator->() const noexcept {
                PPR_ASSERT(m_data != nullptr);
                return m_data;
            }

            [[nodiscard]] constexpr const Dict &operator*() const noexcept {
                PPR_ASSERT(m_data != nullptr);
                return *m_data;
            }

            [[nodiscard]] constexpr const KeyValue &operator[](const std::size_t index) const noexcept {
                PPR_ASSERT(m_data != nullptr);
                return (*m_data)[index];
            }

            [[nodiscard]] constexpr const Value &operator[](const string_literal key) const noexcept {
                PPR_ASSERT(m_data != nullptr);
                return (*m_data)[key];
            }

            [[nodiscard]] constexpr const KeyValue *begin() const noexcept {
                return m_data ? m_data->begin() : nullptr;
            }

            [[nodiscard]] constexpr const KeyValue *end() const noexcept {
                return m_data ? m_data->end() : nullptr;
            }

            constexpr Block() noexcept = default;

            template<mem::details::TArenaAllocator ArenaT>
            explicit Block(const opaque::Dict &init, ArenaT &arena) noexcept {
                resetAssumeEmpty(init, arena);
            }

            template<mem::details::TArenaAllocator ArenaT>
            void resetAssumeEmpty(const opaque::Dict &init, ArenaT &arena) noexcept;

            [[nodiscard]] static constexpr std::size_t sizeOf(const opaque::Dict &dict) noexcept;

            [[nodiscard]] static constexpr std::size_t sizeOf(const opaque::Value &value) noexcept;
        };

        template<mem::details::TArenaAllocator ArenaT>
        void Block::resetAssumeEmpty(const opaque::Dict &init, ArenaT &arena) noexcept {
            PPR_ASSERT(m_data == nullptr);
            m_data = arena.template allocate<Dict>();

            auto dict = arena.template span<KeyValue>(init.size());
            std::construct_at(m_data, dict);

            for (u32 i = 0u; i < init.size(); ++i) {
                const auto &[src_key, src_value] = init[i];
                auto &[dst_key, dst_value] = *std::construct_at(std::addressof(dict[i]));
                dst_key = arena.dup(src_key);
                Builder{dst_value, arena}.dup(src_value);
            }
        }

        constexpr std::size_t Block::sizeOf(const opaque::Dict &dict) noexcept {
            std::size_t size_bytes = sizeof(opaque::Dict);
            size_bytes += alignForward(dict.size() * sizeof(KeyValue), max_align_v);

            for (const auto &[src_key, src_value]: dict) {
                size_bytes += alignForward(src_key.size() * sizeof(*src_key.data()), max_align_v);
                size_bytes += sizeOf(src_value);
            }

            return size_bytes;
        }

        constexpr std::size_t Block::sizeOf(const opaque::Value &value) noexcept {
            return std::visit(
                overloaded(
                    []<typename NumericT>(const NumericT) constexpr noexcept -> std::size_t requires
                        std::integral<NumericT> || std::floating_point<NumericT> {
                        return 0u;
                    },
                    []<pP::details::TChar CharT>(const std::basic_string_view<CharT> str) constexpr noexcept -> std::size_t {
                        return str.size() * sizeof(CharT);
                    },
                    [](const opaque::Array arr) constexpr noexcept -> std::size_t {
                        std::size_t size_bytes = alignForward(arr.size() * sizeof(*arr.data()), max_align_v);
                        for (const opaque::Value &it: arr) {
                            size_bytes += sizeOf(it);
                        }
                        return size_bytes;
                    },
                    [](const opaque::Dict dict) constexpr noexcept -> std::size_t {
                        std::size_t size_bytes = alignForward(dict.size() * sizeof(*dict.data()), max_align_v);
                        for (const auto &[it_key, it_value]: dict) {
                            size_bytes += alignForward(it_key.size() * sizeOf(*it_key.data()), max_align_v);
                            size_bytes += sizeOf(it_value);
                        }
                        return size_bytes;
                    },
                    [](const Formatter &fmt) constexpr noexcept -> std::size_t {
                        // Concrete sink wrapping whatever output iterator this context uses.
                        // Only count memory used to reserve the whole block.
                        struct format_count final : format_context {
                            std::size_t m_count{};

                            void write(const std::string_view sv) override {
                                m_count += sv.size();
                            }
                        };

                        format_count sink{};
                        fmt(sink);
                        return alignForward(sink.m_count * sizeof(char), max_align_v);
                    }), value);
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

    template<>
    struct details::relocatable<opaque::Block> : std::true_type {
    };

    template<>
    struct details::relocatable<opaque::Block::Value> : std::true_type {
    };

    template<>
    struct details::relocatable<opaque::Block::KeyValue> : std::true_type {
    };

    template<>
    struct details::relocatable<opaque::Block::Dict> : std::true_type {
    };
}

// --------------------------------------------------------------
// opaque value formatting
// --------------------------------------------------------------

export namespace std {
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
            sink.write(PPR_LITERAL_FOR(CharT, "\""));
            fmt(sink);
            sink.write(PPR_LITERAL_FOR(CharT, "\""));
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
    struct formatter<pP::opaque::Block::Value, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::opaque::Block::Value &value, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            return visit(
                pP::overloaded(
                    [&]<pP::details::TChar StringCharT>(const pP::relative_view<StringCharT> &inner_value) {
                        return format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:?}"),
                                         std::basic_string_view<StringCharT>(inner_value.data(), inner_value.size()));
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

        static constexpr void set_brackets(basic_string_view<CharT>, basic_string_view<CharT>) noexcept {
        }

        static constexpr void set_separator(basic_string_view<CharT>) noexcept {
        }
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Block::KeyValue, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::opaque::Block::KeyValue &pair, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            return format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:?}: {:}"),
                             std::string_view(pair.first.data(), pair.first.size()),
                             pair.second);
        }

        static constexpr void set_brackets(basic_string_view<CharT>, basic_string_view<CharT>) noexcept {
        }

        static constexpr void set_separator(basic_string_view<CharT>) noexcept {
        }
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

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Block::Dict, CharT>
            : range_formatter<pP::opaque::Block::KeyValue, CharT> {
        using super_t = range_formatter<pP::opaque::Block::KeyValue, CharT>;

        constexpr formatter() noexcept {
            constexpr std::basic_string_view<CharT> bracket_open = PPR_LITERAL_FOR(CharT, "{");
            constexpr std::basic_string_view<CharT> bracket_close = PPR_LITERAL_FOR(CharT, "}");
            super_t::set_brackets(bracket_open, bracket_close);
        }
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::opaque::Block, CharT>
            : formatter<pP::opaque::Block::Dict, CharT> {
        using super_t = formatter<pP::opaque::Block::Dict, CharT>;
        using super_t::super_t;

        template<typename FormatContextT>
        auto format(const pP::opaque::Block &block, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            return super_t::format(*block.m_data, ctx);
        }
    };
}
