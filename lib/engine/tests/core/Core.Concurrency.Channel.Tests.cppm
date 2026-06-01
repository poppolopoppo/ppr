module;
#include "pP/Macros.h"
export module engine.tests:core_channel;
import engine.core;
import std;

export namespace pP::tests {
    namespace ChannelRaw {
        PPR_UNIT_TEST(construction_and_state) {
            const RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            PPR_ASSERT(chan.isOpened());
            PPR_ASSERT(!chan.isClosed());
            PPR_ASSERT(!chan.isClosedOrClosing());
        };

        PPR_UNIT_TEST(single_threaded_send_receive) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 42;
            chan.producerSubmit(*hdr);

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(*static_cast<int *>(read->data()) == 42);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(multiple_messages_sequence) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            for (int i = 0; i < 10; ++i) {
                auto hdr = chan.producerReserve(sizeof(int));
                PPR_ASSERT(hdr.has_value());
                *static_cast<int *>(hdr->data()) = i;
                chan.producerSubmit(*hdr);
            }

            for (int i = 0; i < 10; ++i) {
                auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_ASSERT(hdr.has_value());
                PPR_ASSERT(*static_cast<int *>(hdr->data()) == i);
                chan.consumerRelease(*hdr);
            }

            const auto empty = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(empty.error() == RawChannel::error_empty);
        };

        PPR_UNIT_TEST(ring_buffer_wrap_around) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            constexpr std::size_t payload_size = 512u;
            constexpr std::size_t record_size = RawChannel::alignSize(payload_size);
            const std::size_t batch_size = chan.capacity() / record_size / 2u;

            // Write first batch
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.producerReserve(payload_size);
                PPR_ASSERT(hdr.has_value());
                std::memset(hdr->data(), static_cast<int>(i & 0xFF), payload_size);
                chan.producerSubmit(*hdr);
            }

            // Read and verify first batch — frees space at the front
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_ASSERT(hdr.has_value());
                const auto *data = static_cast<const std::byte *>(hdr->data());
                for (std::size_t j = 0; j < payload_size; ++j) {
                    PPR_ASSERT(data[j] == static_cast<std::byte>(i & 0xFF));
                }
                chan.consumerRelease(*hdr);
            }

            // Write second batch — wraps around in the ring buffer
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.producerReserve(payload_size);
                PPR_ASSERT(hdr.has_value());
                std::memset(hdr->data(), static_cast<int>((i + batch_size) & 0xFF), payload_size);
                chan.producerSubmit(*hdr);
            }

            // Read and verify second batch
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_ASSERT(hdr.has_value());
                const auto *data = static_cast<const std::byte *>(hdr->data());
                for (std::size_t j = 0; j < payload_size; ++j) {
                    PPR_ASSERT(data[j] == static_cast<std::byte>((i + batch_size) & 0xFF));
                }
                chan.consumerRelease(*hdr);
            }
        };

        PPR_UNIT_TEST(backpressure_drop_if_full) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            const std::size_t payload = 64u;

            int count = 0;
            while (auto hdr = chan.producerReserve(payload, RawChannel::drop_if_full)) {
                *static_cast<int *>(hdr->data()) = count;
                chan.producerSubmit(*hdr);
                ++count;
            }
            PPR_ASSERT(count > 0);

            for (int i = 0; i < count; ++i) {
                auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_ASSERT(read.has_value());
                PPR_ASSERT(*static_cast<int *>(read->data()) == i);
                chan.consumerRelease(*read);
            }
        };

        PPR_UNIT_TEST(backpressure_yield_if_full) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            constexpr std::size_t payload = 64u;

            while (auto hdr = chan.producerReserve(payload, RawChannel::drop_if_full)) {
                std::memset(hdr->data(), 0xCC, payload);
                chan.producerSubmit(*hdr);
            }

            std::jthread consumer([&chan] {
                const auto hdr = chan.consumerAcquire();
                PPR_ASSERT(hdr.has_value());
                chan.consumerRelease(*hdr);
            });

            auto hdr = chan.producerReserve(payload, RawChannel::yield_if_full);
            PPR_ASSERT(hdr.has_value());
            std::memset(hdr->data(), 0xDD, payload);
            chan.producerSubmit(*hdr);

            consumer.join();
        };

        PPR_UNIT_TEST(backpressure_wait_if_full) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            constexpr std::size_t payload = 64u;

            while (auto hdr = chan.producerReserve(payload, RawChannel::drop_if_full)) {
                std::memset(hdr->data(), 0xEE, payload);
                chan.producerSubmit(*hdr);
            }

            PPR_ASSERT(chan.producerReserve(payload, RawChannel::drop_if_full).error() == RawChannel::error_full);

            std::jthread consumer([&chan] {
                const auto hdr = chan.consumerAcquire();
                PPR_ASSERT(hdr.has_value());
                chan.consumerRelease(*hdr);
            });

            auto hdr = chan.producerReserve(payload, RawChannel::wait_if_full);
            PPR_ASSERT(hdr.has_value());
            std::memset(hdr->data(), 0xFF, payload);
            chan.producerSubmit(*hdr);

            consumer.join();
        };

        PPR_UNIT_TEST(discard_record) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr1 = chan.producerReserve(sizeof(int));
            PPR_ASSERT(hdr1.has_value());
            *static_cast<int *>(hdr1->data()) = 100;
            chan.producerDiscard(*hdr1);

            auto hdr2 = chan.producerReserve(sizeof(int));
            PPR_ASSERT(hdr2.has_value());
            *static_cast<int *>(hdr2->data()) = 200;
            chan.producerSubmit(*hdr2);

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(*static_cast<int *>(read->data()) == 200);
            chan.consumerRelease(*read);

            read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.error() == RawChannel::error_empty);
        };

        PPR_UNIT_TEST(flush_roundtrip) {
            std::atomic<int> test_phase = 0u;
            PPR_DEFER {
                PPR_ASSERT(test_phase == 3u);
            };

            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 999;
            chan.producerSubmit(*hdr);

            std::jthread consumer([&] {
                test_phase = 1u;

                auto read = chan.consumerAcquire();
                PPR_ASSERT(read.has_value());
                PPR_ASSERT(*static_cast<int *>(read->data()) == 999);
                chan.consumerRelease(*read);

                test_phase = 2u;

                read = chan.consumerAcquire();
                PPR_ASSERT(not read.has_value());

                test_phase = 3u;
            });

            const auto flush = chan.flush();
            PPR_ASSERT(flush.has_value());

            PPR_ASSERT(test_phase == 2u);

            const auto close = chan.close();
            PPR_ASSERT(close.has_value());
        };

        PPR_UNIT_TEST(peek_without_blocking_empty) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            const auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(hdr.error() == RawChannel::error_empty);
        };

        PPR_UNIT_TEST(peek_without_blocking_with_data) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 777;
            chan.producerSubmit(*hdr);

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(*static_cast<int *>(read->data()) == 777);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(close_idempotent) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            PPR_ASSERT(chan.isOpened());

            const auto close = chan.close();
            PPR_ASSERT(close.has_value());
            PPR_ASSERT(chan.isClosedOrClosing());

            const auto close2 = chan.close();
            PPR_ASSERT(close2.error() == RawChannel::error_closed);
            PPR_ASSERT(chan.isClosedOrClosing());
        };

        PPR_UNIT_TEST(close_record_consumed) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 55;
            chan.producerSubmit(*hdr);

            const auto close = chan.close();
            PPR_ASSERT(close.has_value());
            PPR_ASSERT(chan.isClosedOrClosing());

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(*static_cast<int *>(read->data()) == 55);
            chan.consumerRelease(*read);

            read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.error() == RawChannel::error_closed);
            PPR_ASSERT(chan.isClosed());
        };

        PPR_UNIT_TEST(record_header_accessors) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            const char payload[] = "payload";
            auto hdr = chan.producerReserve(sizeof(payload));
            PPR_ASSERT(hdr.has_value());

            PPR_ASSERT(hdr->size() == alignForward(sizeof(payload), max_align_v));
            std::memcpy(hdr->data(), payload, sizeof(payload));

            const void *data_ptr = hdr->data();
            PPR_ASSERT(data_ptr != nullptr);

            const auto const_hdr = hdr;
            const void *const_data_ptr = const_hdr->data();
            PPR_ASSERT(const_data_ptr == data_ptr);

            const auto alloc_result = hdr->allocation();
            PPR_ASSERT(alloc_result.ptr == data_ptr);
            PPR_ASSERT(alloc_result.count == hdr->size());

            chan.producerSubmit(*hdr);

            const auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(std::memcmp(read->data(), &payload, sizeof(payload)) == 0);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(zero_size_record) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(0u);
            PPR_ASSERT(hdr.has_value());
            PPR_ASSERT(hdr->size() == 0u);
            chan.producerSubmit(*hdr);

            const auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(read->size() == 0u);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(consumer_blocks_until_data) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            std::jthread producer([&chan] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto hdr = chan.producerReserve(sizeof(int));
                PPR_ASSERT(hdr.has_value());
                *static_cast<int *>(hdr->data()) = 12345;
                chan.producerSubmit(*hdr);
            });

            auto read = chan.consumerAcquire();
            PPR_ASSERT(read.has_value());
            PPR_ASSERT(*static_cast<int *>(read->data()) == 12345);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(concurrent_spsc) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            constexpr int num_messages = 1000;

            std::jthread producer([&chan] {
                for (int i = 0; i < num_messages; ++i) {
                    auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
                    PPR_ASSERT(hdr.has_value());
                    *static_cast<int *>(hdr->data()) = i;
                    chan.producerSubmit(*hdr);
                }
            });

            for (int i = 0; i < num_messages; ++i) {
                auto hdr = chan.consumerAcquire();
                PPR_ASSERT(hdr.has_value());
                PPR_ASSERT(*static_cast<int *>(hdr->data()) == i);
                chan.consumerRelease(*hdr);
            }

            PPR_VERIFY(chan.close().has_value());
        };

        PPR_UNIT_TEST(concurrent_mpsc) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            constexpr int messages_per_thread = 500;
            constexpr int num_producers = 4;

            std::vector<std::jthread> producers;
            producers.reserve(num_producers);

            std::atomic<int> seed_send = 0;

            std::barrier close_barrier{
                num_producers, [&]() noexcept {
                    const auto close = chan.close();
                    PPR_ASSERT(close.has_value());
                }
            };

            for (int t = 0; t < num_producers; ++t) {
                producers.emplace_back([=, &chan, &seed_send, &close_barrier]() mutable {
                    int local_send = 0;
                    PPR_DEFER {
                        seed_send += local_send;
                    };
                    for (int i = 0; i < messages_per_thread; ++i) {
                        auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
                        PPR_ASSERT(hdr.has_value());

                        auto *const p_value = static_cast<int *>(hdr->data());
                        *p_value = t * 10000 + i;
                        local_send += *p_value;
                        chan.producerSubmit(*hdr);
                    }

                    close_barrier.arrive_and_drop();
                });
            }

            int received = 0;
            int seed_recv = 0u;

            while (true) {
                auto hdr = chan.consumerAcquire(RawChannel::block_until_available);
                if (hdr.has_value()) {
                    const int val = *static_cast<const int *>(hdr->data());
                    seed_recv += val;
                    chan.consumerRelease(*hdr);

                    ++received;
                } else if (hdr.error() == RawChannel::error_closed) {
                    break;
                } else {
                    PPR_ASSERT(false && "branch should not be reachable");
                }
            }

            constexpr int total = num_producers * messages_per_thread;
            PPR_ASSERT(received == total);
            const int local_seed_send = seed_send.load();
            PPR_ASSERT(local_seed_send == seed_recv);

            for (auto &p: producers) {
                p.join();
            }
        };

        PPR_UNIT_TEST(concurrent_mpmc) {
            constexpr int messages_per_thread = 500;
            constexpr int num_producers = 8;
            constexpr int num_consumers = 4;

            std::array<RawChannel, num_consumers> channels;
            std::atomic<int> channels_fan_out{0};

            std::vector<std::jthread> producers;
            producers.reserve(num_producers);

            std::atomic<int> seed_send = 0u;

            for (int t = 0; t < num_producers; ++t) {
                producers.emplace_back([=, &channels, &seed_send, &channels_fan_out]() mutable {
                    int local_send = 0;
                    PPR_DEFER {
                        seed_send += local_send;
                    };
                    for (int i = 0; i < messages_per_thread; ++i) {
                        RawChannel &chan = channels[channels_fan_out.fetch_add(1) % num_consumers];
                        auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
                        PPR_ASSERT(hdr.has_value());
                        auto *const p_value = static_cast<int *>(hdr->data());
                        *p_value = t * 10000 + i;
                        local_send += *p_value;
                        chan.producerSubmit(*hdr);
                    }
                });
            }

            std::vector<std::jthread> consumers;
            consumers.reserve(num_consumers);

            std::atomic<int> received = 0;
            std::atomic<int> seed_recv = 0u;

            for (int t = 0; t < num_consumers; ++t) {
                consumers.emplace_back([=, &channels, &seed_recv, &received] {
                    RawChannel &chan = channels[t];
                    while (true) {
                        auto hdr = chan.consumerAcquire(RawChannel::block_until_available);
                        if (hdr.has_value()) {
                            const int val = *static_cast<const int *>(hdr->data());
                            seed_recv += val;
                            chan.consumerRelease(*hdr);
                            received += 1u;
                        } else if (hdr.error() == RawChannel::error_closed) {
                            break;
                        } else {
                            PPR_ASSERT(false && "branch should not be reachable");
                        }
                    }
                });
            }

            for (auto &p: producers) {
                p.join();
            }

            for (RawChannel &chan: channels) {
                const auto flush = chan.flush();
                PPR_ASSERT(flush.has_value());
            }

            constexpr int total = num_producers * messages_per_thread;
            PPR_ASSERT(received.load() == total);
            PPR_ASSERT(seed_send.load() == seed_recv.load());

            for (RawChannel &chan: channels) {
                const auto close = chan.close();
                PPR_ASSERT(close.has_value());
            }

            for (auto &p: consumers) {
                p.join();
            }
        };

        PPR_UNIT_TEST(concurrent_close_wakeup) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            std::jthread consumer([&chan] {
                const auto hdr = chan.consumerAcquire();
                PPR_ASSERT(not hdr.has_value());
                PPR_ASSERT(hdr.error() == RawChannel::error_closed);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto close = chan.close();
            PPR_ASSERT(close.has_value());
        };
    }

    namespace ChannelTyped {
        namespace details {
            struct MoveOnlyType {
                int value{};

                explicit MoveOnlyType(const int v) noexcept : value(v) {
                }

                MoveOnlyType(const MoveOnlyType &) = delete;

                MoveOnlyType &operator=(const MoveOnlyType &) = delete;

                MoveOnlyType(MoveOnlyType &&other) noexcept : value(std::exchange(other.value, -1)) {
                }

                MoveOnlyType &operator=(MoveOnlyType &&other) noexcept {
                    value = std::exchange(other.value, -1);
                    return *this;
                }
            };

            struct TrackedDestruction {
                inline static std::size_t count = 0u;

                int value{};

                explicit TrackedDestruction(const int v) noexcept : value(v) {
                }

                ~TrackedDestruction() noexcept {
                    ++count;
                }

                TrackedDestruction(const TrackedDestruction &) = default;

                TrackedDestruction(TrackedDestruction &&other) noexcept = default;

                TrackedDestruction &operator=(const TrackedDestruction &) = default;

                TrackedDestruction &operator=(TrackedDestruction &&other) noexcept = default;
            };
        }

        PPR_UNIT_TEST(send_and_receive_int) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_VERIFY(chan.emplace(42).has_value());
            const auto result = chan.peek();
            PPR_ASSERT(result.has_value());
            PPR_ASSERT(*result == 42);
        };

        PPR_UNIT_TEST(emplace_construction) {
            auto chan = pP::Channel<std::string>(std::in_place_t{}, 16u);

            const auto sent = chan.send(std::string("hello world"));
            PPR_ASSERT(sent.has_value());

            const auto result = chan.peek();
            PPR_ASSERT(result.has_value());
            PPR_ASSERT(result.value() == "hello world");
        };

        PPR_UNIT_TEST(peek_non_blocking) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            const auto empty = chan.peek();
            PPR_ASSERT(!empty.has_value());

            const auto sent = chan.send(99);
            PPR_ASSERT(sent.has_value());

            const auto val = chan.peek();
            PPR_ASSERT(val.has_value());
            PPR_ASSERT(val.value() == 99);
        };

        PPR_UNIT_TEST(send_receive_string) {
            auto chan = pP::Channel<std::string>(std::in_place_t{}, 8u);

            std::string msg = "test message";
            PPR_VERIFY(chan.emplace(std::move(msg)).has_value());
            PPR_ASSERT(msg.empty());

            const auto result = chan.peek();
            PPR_ASSERT(result.has_value());
            PPR_ASSERT(result.value() == "test message");
        };

        PPR_UNIT_TEST(operator_stream_send) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            auto err = chan << 10 << 20 << 30;
            PPR_ASSERT(not err.has_error());

            const auto r1 = chan.peek();
            PPR_ASSERT(r1.value() == 10);
            const auto r2 = chan.peek();
            PPR_ASSERT(r2.value() == 20);
            const auto r3 = chan.peek();
            PPR_ASSERT(r3.value() == 30);
        };

        PPR_UNIT_TEST(operator_stream_receive) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            auto sent = chan.send(42);
            PPR_ASSERT(sent.has_value());

            sent = chan.send(84);
            PPR_ASSERT(sent.has_value());

            int val = 0;
            PPR_VERIFY((chan >> val).has_value());
            PPR_ASSERT(val == 42);
            PPR_VERIFY((chan >> val).has_value());
            PPR_ASSERT(val == 84);
        };

        PPR_UNIT_TEST(in_place_construction) {
            pP::Channel<int> chan{std::in_place_t{}, 32u};

            auto sent = chan.send(1);
            PPR_ASSERT(sent.has_value());
            sent = chan.send(2);
            PPR_ASSERT(sent.has_value());
            sent = chan.send(3);
            PPR_ASSERT(sent.has_value());

            PPR_VERIFY(chan.receive().value() == 1);
            PPR_VERIFY(chan.receive().value() == 2);
            PPR_VERIFY(chan.receive().value() == 3);
        };

        PPR_UNIT_TEST(close_propagation) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_VERIFY(chan.send(1).has_value());

            const auto close = chan.close();
            PPR_ASSERT(close.has_value());

            const auto recv = chan.receive();
            PPR_ASSERT(recv.has_value());
            PPR_ASSERT(recv.value() == 1);

            const auto peak = chan.peek();
            PPR_VERIFY(not peak.has_value());

            PPR_VERIFY(not chan.send(2));
        };

        PPR_UNIT_TEST(backpressure_drop) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 2u);

            int sent = 0;
            for (int i = 1; chan.send(i); i++) {
                sent += i;
            }

            int received = 0;
            while (auto opt = chan.peek()) {
                PPR_ASSERT(opt.has_value());
                received += opt.value();
            }

            PPR_ASSERT(sent == received);
        };

        PPR_UNIT_TEST(destructor_called_on_consume) {
            details::TrackedDestruction::count = 0u;

            auto chan = pP::Channel<details::TrackedDestruction>(std::in_place_t{}, 8u);
            PPR_VERIFY(chan.emplace(42).has_value());

            const auto result = chan.receive();
            PPR_ASSERT(result.has_value());
            PPR_ASSERT(result.value().value == 42);

            const std::size_t n = details::TrackedDestruction::count;
            PPR_ASSERT(n == 1u);

            const auto close = chan.close();
            PPR_ASSERT(close.has_value());
        };

        PPR_UNIT_TEST(concurrent_channel) {
            pP::Channel<int> chan{std::in_place_t{}, 64u};
            constexpr int num_messages = 1000;

            std::jthread producer([&chan] {
                for (int i = 0; i < num_messages; ++i) {
                    while (not chan.emplace(i)) {
                        std::this_thread::yield();
                    }
                }
            });

            for (int i = 0; i < num_messages; ++i) {
                auto result = chan.receive();
                PPR_ASSERT(result.has_value());
                PPR_ASSERT(result.value() == i);
            }

            PPR_VERIFY(chan.close().has_value());
        };

        PPR_UNIT_TEST(operator_receive_on_closed) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);
            PPR_VERIFY(chan.close().has_value());

            int val = 0;
            PPR_VERIFY(not (chan >> val));
        };

        PPR_UNIT_TEST(operator_receive_empty) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            int count = 0;
            for (auto it = chan.begin(RawChannel::peek_without_blocking); it != chan.end(); ++it) {
                ++count;
            }
            PPR_ASSERT(count == 0);
        };

        PPR_UNIT_TEST(shared_ptr_construction) {
            auto raw = std::make_shared<RawChannel>(static_cast<std::size_t>(hal::page_granularity));
            auto chan = pP::Channel<int>(std::move(raw));

            const auto sent = chan.send(777);
            PPR_ASSERT(sent.has_value());

            const auto result = chan.receive();
            PPR_ASSERT(result.has_value());
            PPR_ASSERT(result.value() == 777);
        };

        PPR_UNIT_TEST(flush) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_VERIFY(chan.send(1).has_value());
            PPR_VERIFY(chan.send(2).has_value());

            std::atomic<int> total{0};

            std::jthread consumer([&] {
                const auto r1 = chan.receive();
                PPR_ASSERT(r1.value() == 1);
                total += r1.value();
                const auto r2 = chan.receive();
                PPR_ASSERT(r2.value() == 2);
                total += r2.value();
                const auto r3 = chan.receive();
                PPR_ASSERT(not r3);
            });

            PPR_VERIFY(!!chan.flush());
            PPR_ASSERT(total == 3);
            PPR_VERIFY(!!chan.close());
        };

        PPR_UNIT_TEST(auto_close_in_destructor) {
            {
                const RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
                PPR_ASSERT(chan.isOpened());
            }
        };

        PPR_UNIT_TEST(channel_destructor_drains_non_trivial) {
            details::TrackedDestruction::count = 0u;
            {
                auto chan = pP::Channel<details::TrackedDestruction>(std::in_place_t{}, 8u);
                PPR_VERIFY(chan.emplace(42).has_value());
                PPR_VERIFY(chan.emplace(99).has_value());
                const auto r1 = chan.receive();
                PPR_ASSERT(r1.has_value());
                PPR_ASSERT(r1->value == 42);
                const auto r2 = chan.receive();
                PPR_ASSERT(r2.has_value());
                PPR_ASSERT(r2->value == 99);
                const std::size_t n = details::TrackedDestruction::count;
                PPR_ASSERT(n == 2u);
            }
            const std::size_t n = details::TrackedDestruction::count;
            PPR_ASSERT(n == 4u);
        };

        PPR_UNIT_TEST(range_iteration_blocking) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            std::jthread producer([&chan] {
                for (int i = 0; i < 5; ++i) {
                    PPR_VERIFY(chan.emplace(i).has_value());
                }
                PPR_VERIFY(chan.close().has_value());
            });

            int expected = 0;
            for (const auto &msg: chan) {
                PPR_ASSERT(msg == expected++);
            }
            PPR_ASSERT(expected == 5);
        };

        PPR_UNIT_TEST(range_iteration_non_blocking) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_VERIFY(chan.emplace(1).has_value());
            PPR_VERIFY(chan.emplace(2).has_value());
            PPR_VERIFY(chan.emplace(3).has_value());
            PPR_VERIFY(chan.close().has_value());

            int sum = 0;
            for (auto it = chan.begin(RawChannel::EPolling::peek_without_blocking); it != chan.end(); ++it) {
                sum += *it;
            }
            PPR_ASSERT(sum == 6);
        };
    }

    PPR_UNIT_TEST(raw_channel) {
        _.recurse(ChannelRaw::construction_and_state);
        _.recurse(ChannelRaw::single_threaded_send_receive);
        _.recurse(ChannelRaw::multiple_messages_sequence);
        _.recurse(ChannelRaw::ring_buffer_wrap_around);
        _.recurse(ChannelRaw::backpressure_drop_if_full);
        _.recurse(ChannelRaw::backpressure_yield_if_full);
        _.recurse(ChannelRaw::backpressure_wait_if_full);
        _.recurse(ChannelRaw::discard_record);
        _.recurse(ChannelRaw::flush_roundtrip);
        _.recurse(ChannelRaw::peek_without_blocking_empty);
        _.recurse(ChannelRaw::peek_without_blocking_with_data);
        _.recurse(ChannelRaw::close_idempotent);
        _.recurse(ChannelRaw::close_record_consumed);
        _.recurse(ChannelRaw::record_header_accessors);
        _.recurse(ChannelRaw::zero_size_record);
        _.recurse(ChannelRaw::consumer_blocks_until_data);
        _.recurse(ChannelRaw::concurrent_spsc);
        _.recurse(ChannelRaw::concurrent_mpsc);
        _.recurse(ChannelRaw::concurrent_mpmc);
        _.recurse(ChannelRaw::concurrent_close_wakeup);
    };

    PPR_UNIT_TEST(typed_channel) {
        _.recurse(ChannelTyped::send_and_receive_int);
        _.recurse(ChannelTyped::emplace_construction);
        _.recurse(ChannelTyped::peek_non_blocking);
        _.recurse(ChannelTyped::send_receive_string);
        _.recurse(ChannelTyped::operator_stream_send);
        _.recurse(ChannelTyped::operator_stream_receive);
        _.recurse(ChannelTyped::in_place_construction);
        _.recurse(ChannelTyped::close_propagation);
        _.recurse(ChannelTyped::backpressure_drop);
        _.recurse(ChannelTyped::destructor_called_on_consume);
        _.recurse(ChannelTyped::concurrent_channel);
        _.recurse(ChannelTyped::operator_receive_on_closed);
        _.recurse(ChannelTyped::operator_receive_empty);
        _.recurse(ChannelTyped::shared_ptr_construction);
        _.recurse(ChannelTyped::flush);
        _.recurse(ChannelTyped::auto_close_in_destructor);
        _.recurse(ChannelTyped::channel_destructor_drains_non_trivial);
        _.recurse(ChannelTyped::range_iteration_blocking);
        _.recurse(ChannelTyped::range_iteration_non_blocking);
    };

    PPR_UNIT_TEST(channel) {
        _.recurse(raw_channel);
        _.recurse(typed_channel);
    };
}
