// ReSharper disable CppPolymorphicClassWithNonVirtualPublicDestructor
module;
#include "pP/Macros.h"
export module engine.core:concurrency.channel;

import :assert;
import :concurrency.event;
import :hal;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // a channel is a lock-free MPSC circular buffer for fast message passing between threads
    // ------------------------------------------------------------------

    // inspired from BPF ring buffer: https://www.kernel.org/doc/html/latest/bpf/ringbuf.html

    PPR_PRAGMA_WARNING_PUSH()
    PPR_PRAGMA_WARNING_DISABLE_MSVC(4324) // ignore padding due to cache-line alignment
    PPR_PRAGMA_WARNING_DISABLE_MSVC(4265) // ignore the absence of virtual destructor for this class

    class alignas(hal::cacheline_size_v) RawChannel : public IEvent {
        void advanceCommit_() noexcept;

        void *const m_data{};
        const std::size_t m_capacity{};

        std::mutex m_producer_mutex{};
        PulseEvent m_on_produced{};
        std::size_t m_write{};

        alignas(hal::cacheline_size_v) std::atomic<std::size_t> m_commit{};
        alignas(hal::cacheline_size_v) std::atomic<std::size_t> m_read{};

        enum EStatus_ {
            status_opened,
            status_closing,
            status_closed,
        };

        std::atomic<int> m_status{status_opened};

    protected:
        struct alignas(u64) RecordHeader {
            enum EFlags : u32 {
                none = 0u,

                flag_busy = 0b0001u,
                flag_discard = 0b0010u,
                flag_flush = 0b0100u,
                flag_close = 0b1000u,

                all = flag_busy | flag_discard | flag_flush | flag_close,
            };

            EFlags m_flags{};
            u32 m_available_size{};

            [[nodiscard]] void *data() & noexcept {
                return this + 1;
            }

            [[nodiscard]] const void *data() const & noexcept {
                return this + 1;
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return m_available_size;
            }

            [[nodiscard]] std::allocation_result<void *> allocation() & noexcept {
                return {this + 1, m_available_size};
            }

            [[nodiscard]] bool isClosing() const noexcept {
                return (m_flags & flag_close) != none;
            }
        };

    public:
        [[nodiscard]] static constexpr std::size_t alignSize(const std::size_t size_bytes) noexcept {
            return sizeof(RecordHeader) + alignForward(size_bytes, alignof(RecordHeader));
        }

        struct Record {
            std::reference_wrapper<RecordHeader> m_header;

            // ReSharper disable once CppNonExplicitConversionOperator
            [[nodiscard]] RecordHeader &get() const noexcept {
                return m_header;
            }

            // ReSharper disable once CppNonExplicitConversionOperator
            [[nodiscard]] operator RecordHeader &() const noexcept {
                return m_header;
            }

            [[nodiscard]] void *data() noexcept {
                return m_header.get().data();
            }

            [[nodiscard]] const void *data() const noexcept {
                return m_header.get().data();
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return m_header.get().size();
            }

            [[nodiscard]] std::allocation_result<void *> allocation() const noexcept {
                return m_header.get().allocation();
            }
        };

        explicit RawChannel(const std::size_t buffer_size) noexcept;
        explicit RawChannel() noexcept;
        RawChannel(const std::size_t num_elements, const std::size_t element_size) noexcept;
        virtual ~RawChannel() noexcept;

        RawChannel(const RawChannel &) = delete;

        RawChannel &operator=(const RawChannel &) = delete;

        RawChannel(const RawChannel &&) = delete;

        RawChannel &operator=(const RawChannel &&) = delete;

        [[nodiscard]] bool isOpened() const noexcept;
        [[nodiscard]] bool isClosed() const noexcept;
        [[nodiscard]] bool isClosedOrClosing() const noexcept;
        [[nodiscard]] std::size_t capacity() const noexcept;

        enum errc : int {
            error_closed = 1,
            error_empty,
            error_full,
            error_invalid,
        };

        [[nodiscard]] std::error_code flush() noexcept;

        [[nodiscard]] std::error_code close() noexcept;

        enum EBackPressure {
            drop_if_full,
            wait_if_full,
            yield_if_full,
        };

    private:
        [[nodiscard]] std::expected<Record, errc>
        producerReserveAssumeNotClosed_(const std::size_t size_bytes, const EBackPressure policy) noexcept;

    public:
        [[nodiscard]] std::expected<Record, errc>
        producerReserve(const std::size_t size_bytes, const EBackPressure policy = wait_if_full) noexcept;

        void producerSubmit(RecordHeader &written) noexcept;

        void producerDiscard(RecordHeader &written) noexcept;

        enum EPolling {
            block_until_available,
            peek_without_blocking,
        };

        [[nodiscard]] std::expected<Record, errc>
        consumerAcquire(const EPolling policy = block_until_available) noexcept;

        void consumerRelease(const RecordHeader &read) noexcept;

        // ------------------------------------------------------------------
        // IEvent interface implementation
        // ------------------------------------------------------------------

        [[nodiscard]] TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept final;
        void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept final;
        [[nodiscard]] bool pollEvent() noexcept final;
        void resetEvent() noexcept final;
    };

    PPR_PRAGMA_WARNING_POP()
}

template<>
struct std::is_error_code_enum<pP::RawChannel::errc> : std::true_type { // NOLINT(*-dcl58-cpp)
};

export namespace pP {
    [[nodiscard]] const std::error_category &error_category() noexcept;

    [[nodiscard]] std::error_code make_error_code(RawChannel::errc error) noexcept;

    using SharedRawChannelPtr = std::shared_ptr<RawChannel>;



    // ------------------------------------------------------------------
    // typed channel helpers for fixed-size messages
    // ------------------------------------------------------------------

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    class ChannelReader;

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    class ChannelWriter;

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    class Channel {
        SharedRawChannelPtr m_chan;

        using RecordHeader = RawChannel::Record;

    public:
        using EBackPressure = RawChannel::EBackPressure;
        using EError = RawChannel::errc;
        using EPolling = RawChannel::EPolling;

        using enum RawChannel::EBackPressure;
        using enum RawChannel::errc;
        using enum RawChannel::EPolling;

        explicit Channel(SharedRawChannelPtr &&chan) noexcept
            : m_chan{std::move(chan)} {
            PPR_ASSERT(m_chan != nullptr);
        }

        Channel(std::in_place_t, const std::size_t capacity)
            : Channel(std::make_shared<RawChannel>(capacity, sizeof(T))) {
            PPR_ASSERT(capacity > 0);
        }

        ~Channel() noexcept {
            [[maybe_unused]] auto err = close();
            PPR_ASSERT(not err or err == make_error_code(error_closed));

#if PPR_ENABLE_ASSERTIONS
            if constexpr (not std::is_trivially_destructible_v<T>
                          && std::is_nothrow_move_constructible_v<T>) {
                std::size_t unread = 0;
                while (peek().has_value()) {
                    ++unread;
                }
                PPR_ASSERT(unread == 0
                    && "Channel destroyed with unread non-trivially-destructible messages");
            }
#endif
        }

        [[nodiscard]] std::error_code flush() noexcept {
            if (RawChannel *const p_chan = m_chan.get()) [[likely]] {
                return p_chan->flush();
            }
            return make_error_code(error_invalid);
        }

        [[nodiscard]] std::error_code close() noexcept {
            if (RawChannel *const p_chan = m_chan.get()) [[likely]] {
                return p_chan->close();
            }
            return make_error_code(error_invalid);
        }

        [[nodiscard]] ChannelReader<T> reader() noexcept;

        [[nodiscard]] ChannelWriter<T> writer() noexcept;

        template<typename LikeT>
        [[nodiscard]] std::error_code
        send(LikeT &&value, const EBackPressure policy = drop_if_full) noexcept
            requires std::conjunction_v<std::is_nothrow_constructible<T, LikeT &&>, std::is_nothrow_move_constructible<T> > {
            auto hdr = m_chan->producerReserve(sizeof(T), policy);

            if (hdr.has_value()) [[likely]] {
                new(hdr->data()) T(std::forward<LikeT>(value));
                m_chan->producerSubmit(*hdr);
                return {};
            }

            return make_error_code(hdr.error());
        }

        template<typename... ArgsT>
        [[nodiscard]] std::error_code
        emplace(const EBackPressure policy, ArgsT &&... args) noexcept
            requires std::is_nothrow_constructible_v<T, ArgsT &&...> {
            auto hdr = m_chan->producerReserve(sizeof(T), policy);
            if (hdr.has_value()) [[likely]] {
                new(hdr->data()) T(std::forward<ArgsT>(args)...);
                m_chan->producerSubmit(*hdr);
                return {};
            }
            return make_error_code(hdr.error());
        }

        template<typename... ArgsT>
        [[nodiscard]] std::error_code
        emplace(ArgsT &&... args) noexcept
            requires std::is_nothrow_constructible_v<T, ArgsT &&...> {
            return emplace(wait_if_full, std::forward<ArgsT>(args)...);
        }

        template<typename ChannelT>
        class [[nodiscard]] BasicSendResult {
            ChannelT &m_chan;
            std::optional<std::error_code> m_error{std::nullopt};

        public:
            BasicSendResult(ChannelT &chan, const std::error_code &result) noexcept
                : m_chan{chan} {
                if (result) {
                    m_error = result;
                }
            }

            [[nodiscard]] bool has_error() const noexcept {
                return m_error.has_value();
            }

            [[nodiscard]] std::error_code error() const noexcept {
                return m_error.value();
            }

            friend BasicSendResult operator<<(const BasicSendResult &writer, const T &value) noexcept {
                if (not writer.m_error.has_value()) [[likely]] {
                    return writer.m_chan << value;
                }
                return writer;
            }

            friend BasicSendResult operator<<(const BasicSendResult &writer, T &&value) noexcept {
                if (not writer.m_error.has_value()) [[likely]] {
                    return writer.m_chan << std::move(value);
                }
                return writer;
            }
        };

        using SendResult = BasicSendResult<Channel>;

        friend SendResult operator<<(Channel &writer, const T &value) noexcept {
            auto err = writer.send(value);
            return SendResult(writer, err);
        }

        friend SendResult operator<<(Channel &writer, T &&value) noexcept {
            auto err = writer.send(std::move(value));
            return SendResult(writer, err);
        }

        class [[nodiscard]] OutputIterator {
            Channel m_writer;
            EBackPressure m_policy{wait_if_full};
            std::optional<std::error_code> m_error;

        public:
            using iterator_category = std::output_iterator_tag;
            using value_type = void;
            using difference_type = std::ptrdiff_t;
            using pointer = void;
            using reference = void;

            explicit OutputIterator(Channel &writer, const EBackPressure policy = wait_if_full) noexcept
                : m_writer{writer}, m_policy{policy} {
            }

            [[nodiscard]] std::optional<std::error_code> error() const noexcept {
                return m_error;
            }

            OutputIterator &operator*() noexcept {
                return *this;
            }

            OutputIterator &operator=(const T &value) noexcept
                requires std::is_nothrow_copy_constructible_v<T> {
                if (const std::error_code status = m_writer.send(value, m_policy); status) [[unlikely]] {
                    m_error = status;
                }
                return *this;
            }

            OutputIterator &operator=(T &&value) noexcept
                requires std::is_nothrow_move_constructible_v<T> {
                if (const std::error_code status = m_writer.send(value, m_policy); status) [[unlikely]] {
                    m_error = status;
                }
                return *this;
            }

            OutputIterator &operator++() noexcept {
                return *this;
            }

            OutputIterator operator++(int) noexcept {
                return *this;
            }
        };

        [[nodiscard]] OutputIterator output(const EBackPressure policy = wait_if_full) noexcept {
            return OutputIterator{*this, policy};
        }

        [[nodiscard]] std::expected<T, EError>
        receive(const EPolling policy = block_until_available) noexcept
            requires std::is_nothrow_move_constructible_v<T> {
            auto hdr = m_chan->consumerAcquire(policy);

            if (hdr.has_value()) [[likely]] {
                T *const p_written = static_cast<T *>(hdr->data());
                PPR_DEFER {
                    std::destroy_at(p_written);
                    m_chan->consumerRelease(*hdr);
                };
                return std::move(*p_written);
            }

            PPR_ASSERT(hdr.error() == error_closed || policy == peek_without_blocking);
            return std::unexpected(hdr.error());
        }

        [[nodiscard]] std::optional<T> peek() noexcept
            requires std::is_nothrow_move_constructible_v<T> {
            auto it = receive(peek_without_blocking);
            if (it.has_value()) [[likely]] {
                return std::move(*it);
            }
            return std::nullopt;
        }

        friend std::error_code operator>>(Channel &reader, T &dst) noexcept {
            auto result = reader.receive();
            if (result.has_value()) [[likely]] {
                dst = std::move(*result);
                return {};
            }
            return make_error_code(result.error());
        }

        class [[nodiscard]] InputIterator {
            Channel m_reader;
            std::expected<T, EError> m_value{};
            EPolling m_policy{};

            void advance_() noexcept {
                m_value = m_reader.receive(m_policy);
            }

        public:
            using iterator_category = std::input_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using pointer = T *;
            using reference = T &;

            explicit InputIterator(const Channel &reader, const EPolling policy = block_until_available) noexcept
                : m_reader{reader}, m_policy{policy} {
                advance_();
            }

            [[nodiscard]] T &operator*() noexcept {
                PPR_ASSERT(m_value.has_value());
                return *m_value;
            }

            [[nodiscard]] T *operator->() noexcept {
                PPR_ASSERT(m_value.has_value());
                return std::addressof(*m_value);
            }

            InputIterator &operator++() noexcept {
                advance_();
                return *this;
            }

            void operator++(int) noexcept {
                advance_();
            }

            [[nodiscard]] bool isValid() const noexcept {
                return m_reader.m_chan && m_value.has_value();
            }

            friend bool operator==(const InputIterator &a, const InputIterator &b) noexcept {
                return a.m_reader.m_chan == b.m_reader.m_chan;
            }

            friend bool operator!=(const InputIterator &a, const InputIterator &b) noexcept {
                return not(a == b);
            }
        };

        [[nodiscard]] friend bool operator==(const InputIterator &it, std::default_sentinel_t) noexcept {
            return not it.isValid();
        }

        [[nodiscard]] friend bool operator!=(const InputIterator &it, std::default_sentinel_t) noexcept {
            return it.isValid();
        }

        [[nodiscard]] friend bool operator==(std::default_sentinel_t, const InputIterator &it) noexcept {
            return not it.isValid();
        }

        [[nodiscard]] friend bool operator!=(std::default_sentinel_t, const InputIterator &it) noexcept {
            return it.isValid();
        }

        [[nodiscard]] InputIterator begin(EPolling policy = block_until_available) noexcept {
            return InputIterator{*this, policy};
        }

        [[nodiscard]] static std::default_sentinel_t end() noexcept {
            return std::default_sentinel;
        }
    };

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    class ChannelReader {
        Channel<T> m_channel;

    public:
        using EError = typename Channel<T>::EError;
        using EPolling = typename Channel<T>::EPolling;
        using InputIterator = typename Channel<T>::InputIterator;

        using enum RawChannel::errc;
        using enum RawChannel::EPolling;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        ChannelReader(const Channel<T> &channel) noexcept
            : m_channel{channel} {
        }

        [[nodiscard]] std::expected<T, EError>
        receive(const EPolling policy = block_until_available) noexcept
            requires std::is_nothrow_move_constructible_v<T> {
            return m_channel.receive(policy);
        }

        [[nodiscard]] std::optional<T>
        peek() noexcept
            requires std::is_nothrow_move_constructible_v<T> {
            return m_channel.peek(peek_without_blocking);
        }

        [[nodiscard]] friend std::error_code
        operator>>(ChannelReader &reader, T &dst) noexcept {
            return reader.m_channel >> dst;
        }

        [[nodiscard]] InputIterator begin(EPolling policy = block_until_available) noexcept {
            return m_channel.begin(policy);
        }

        [[nodiscard]] static std::default_sentinel_t end() noexcept {
            return std::default_sentinel;
        }
    };

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    class ChannelWriter {
        Channel<T> m_channel;

    public:
        using EBackPressure = typename Channel<T>::EBackPressure;
        using EError = typename Channel<T>::EError;
        using OutputIterator = typename Channel<T>::OutputIterator;
        using SendResult = typename Channel<T>::template BasicSendResult<ChannelWriter>;

        using enum RawChannel::EBackPressure;
        using enum RawChannel::errc;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        ChannelWriter(const Channel<T> &channel) noexcept
            : m_channel{channel} {
        }

        [[nodiscard]] std::error_code flush() noexcept {
            return m_channel.flush();
        }

        [[nodiscard]] std::error_code close() noexcept {
            return m_channel.close();
        }

        template<typename LikeT>
        [[nodiscard]] std::error_code
        send(LikeT &&value) noexcept
            requires std::is_nothrow_constructible_v<T, LikeT &&> {
            return m_channel.send(std::forward<LikeT>(value));
        }

        template<typename... ArgsT>
        [[nodiscard]] std::error_code
        emplace(ArgsT &&... args) noexcept
            requires std::is_nothrow_constructible_v<T, ArgsT &&...> {
            return m_channel.emplace(std::forward<ArgsT>(args)...);
        }

        friend SendResult operator<<(ChannelWriter &writer, const T &value) noexcept {
            return SendResult(writer, writer.m_channel.send(value));
        }

        friend SendResult operator<<(ChannelWriter &writer, T &&value) noexcept {
            return SendResult(writer, writer.m_channel.send(value));
        }

        [[nodiscard]] OutputIterator output(const EBackPressure policy = wait_if_full) noexcept {
            return m_channel.output(policy);
        }
    };

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    [[nodiscard]] ChannelReader<T> Channel<T>::reader() noexcept {
        return ChannelReader<T>(*this);
    }

    template<typename T>
        requires std::is_nothrow_destructible_v<T>
    [[nodiscard]] ChannelWriter<T> Channel<T>::writer() noexcept {
        return ChannelWriter<T>(*this);
    }

    template<typename T>
    using chan = Channel<T>;

    template<typename T>
    using input_chan = ChannelReader<T>;

    template<typename T>
    using output_chan = ChannelWriter<T>;
}
