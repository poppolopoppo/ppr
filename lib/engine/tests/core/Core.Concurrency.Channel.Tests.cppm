module;
#include "pP/UnitTest.h"
export module engine.tests.core:channel;
import engine.core;
import std;

export namespace pP::tests {
    namespace ChannelRaw {
        PPR_UNIT_TEST(construction_and_state) {
            const RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            PPR_TEST_ASSERT(chan.isOpened());
            PPR_TEST_ASSERT(!chan.isClosed());
            PPR_TEST_ASSERT(!chan.isClosedOrClosing());
        };

        PPR_UNIT_TEST(single_threaded_send_receive) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_TEST_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 42;
            chan.producerSubmit(*hdr);

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 42);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(multiple_messages_sequence) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            for (int i = 0; i < 10; ++i) {
                auto hdr = chan.producerReserve(sizeof(int));
                PPR_TEST_ASSERT(hdr.has_value());
                *static_cast<int *>(hdr->data()) = i;
                chan.producerSubmit(*hdr);
            }

            for (int i = 0; i < 10; ++i) {
                auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(hdr.has_value());
                PPR_TEST_ASSERT(*static_cast<int *>(hdr->data()) == i);
                chan.consumerRelease(*hdr);
            }

            const auto empty = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(empty.error() == RawChannel::error_empty);
        };

        PPR_UNIT_TEST(ring_buffer_wrap_around) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            constexpr std::size_t payload_size = 512u;
            constexpr std::size_t record_size = RawChannel::alignSize(payload_size);
            const std::size_t batch_size = chan.capacity() / record_size / 2u;

            // Write first batch
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.producerReserve(payload_size);
                PPR_TEST_ASSERT(hdr.has_value());
                std::memset(hdr->data(), static_cast<int>(i & 0xFF), payload_size);
                chan.producerSubmit(*hdr);
            }

            // Read and verify first batch — frees space at the front
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(hdr.has_value());
                const auto *data = static_cast<const std::byte *>(hdr->data());
                for (std::size_t j = 0; j < payload_size; ++j) {
                    PPR_TEST_ASSERT(data[j] == static_cast<std::byte>(i & 0xFF));
                }
                chan.consumerRelease(*hdr);
            }

            // Write second batch — wraps around in the ring buffer
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.producerReserve(payload_size);
                PPR_TEST_ASSERT(hdr.has_value());
                std::memset(hdr->data(), static_cast<int>((i + batch_size) & 0xFF), payload_size);
                chan.producerSubmit(*hdr);
            }

            // Read and verify second batch
            for (std::size_t i = 0; i < batch_size; ++i) {
                auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(hdr.has_value());
                const auto *data = static_cast<const std::byte *>(hdr->data());
                for (std::size_t j = 0; j < payload_size; ++j) {
                    PPR_TEST_ASSERT(data[j] == static_cast<std::byte>((i + batch_size) & 0xFF));
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
            PPR_TEST_ASSERT(count > 0);

            for (int i = 0; i < count; ++i) {
                auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(read.has_value());
                PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == i);
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
                PPR_TEST_ASSERT(hdr.has_value());
                chan.consumerRelease(*hdr);
            });

            auto hdr = chan.producerReserve(payload, RawChannel::yield_if_full);
            PPR_TEST_ASSERT(hdr.has_value());
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

            PPR_TEST_ASSERT(chan.producerReserve(payload, RawChannel::drop_if_full).error() == RawChannel::error_full);

            std::jthread consumer([&chan] {
                const auto hdr = chan.consumerAcquire();
                PPR_TEST_ASSERT(hdr.has_value());
                chan.consumerRelease(*hdr);
            });

            auto hdr = chan.producerReserve(payload, RawChannel::wait_if_full);
            PPR_TEST_ASSERT(hdr.has_value());
            std::memset(hdr->data(), 0xFF, payload);
            chan.producerSubmit(*hdr);

            consumer.join();
        };

        PPR_UNIT_TEST(discard_record) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr1 = chan.producerReserve(sizeof(int));
            PPR_TEST_ASSERT(hdr1.has_value());
            *static_cast<int *>(hdr1->data()) = 100;
            chan.producerDiscard(*hdr1);

            auto hdr2 = chan.producerReserve(sizeof(int));
            PPR_TEST_ASSERT(hdr2.has_value());
            *static_cast<int *>(hdr2->data()) = 200;
            chan.producerSubmit(*hdr2);

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 200);
            chan.consumerRelease(*read);

            read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.error() == RawChannel::error_empty);
        };

        PPR_UNIT_TEST(flush_roundtrip) {
            std::atomic<int> test_phase = 0u;
            PPR_DEFER {
                PPR_TEST_ASSERT(test_phase == 3u);
            };

            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_TEST_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 999;
            chan.producerSubmit(*hdr);

            std::jthread consumer([&] {
                test_phase = 1u;

                auto read = chan.consumerAcquire();
                PPR_TEST_ASSERT(read.has_value());
                PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 999);
                chan.consumerRelease(*read);

                test_phase = 2u;

                read = chan.consumerAcquire();
                PPR_TEST_ASSERT(not read.has_value());

                test_phase = 3u;
            });

            const auto flush = chan.flush();
            PPR_TEST_ASSERT(flush.has_value());

            PPR_TEST_ASSERT(test_phase == 2u);

            const auto close = chan.close();
            PPR_TEST_ASSERT(close.has_value());
        };

        PPR_UNIT_TEST(peek_without_blocking_empty) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            const auto hdr = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(hdr.error() == RawChannel::error_empty);
        };

        PPR_UNIT_TEST(peek_without_blocking_with_data) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_TEST_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 777;
            chan.producerSubmit(*hdr);

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 777);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(close_idempotent) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            PPR_TEST_ASSERT(chan.isOpened());

            const auto close = chan.close();
            PPR_TEST_ASSERT(close.has_value());
            PPR_TEST_ASSERT(chan.isClosedOrClosing());

            const auto close2 = chan.close();
            PPR_TEST_ASSERT(close2.error() == RawChannel::error_closed);
            PPR_TEST_ASSERT(chan.isClosedOrClosing());
        };

        PPR_UNIT_TEST(close_record_consumed) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(sizeof(int));
            PPR_TEST_ASSERT(hdr.has_value());
            *static_cast<int *>(hdr->data()) = 55;
            chan.producerSubmit(*hdr);

            const auto close = chan.close();
            PPR_TEST_ASSERT(close.has_value());
            PPR_TEST_ASSERT(chan.isClosedOrClosing());

            auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 55);
            chan.consumerRelease(*read);

            read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.error() == RawChannel::error_closed);
            PPR_TEST_ASSERT(chan.isClosed());
        };

        PPR_UNIT_TEST(record_header_accessors) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            const char payload[] = "payload";
            auto hdr = chan.producerReserve(sizeof(payload));
            PPR_TEST_ASSERT(hdr.has_value());

            PPR_TEST_ASSERT(hdr->size() == alignForward(sizeof(payload), max_align_v));
            std::memcpy(hdr->data(), payload, sizeof(payload));

            const void *data_ptr = hdr->data();
            PPR_TEST_ASSERT(data_ptr != nullptr);

            const auto const_hdr = hdr;
            const void *const_data_ptr = const_hdr->data();
            PPR_TEST_ASSERT(const_data_ptr == data_ptr);

            const auto alloc_result = hdr->allocation();
            PPR_TEST_ASSERT(alloc_result.ptr == data_ptr);
            PPR_TEST_ASSERT(alloc_result.count == hdr->size());

            chan.producerSubmit(*hdr);

            const auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(std::memcmp(read->data(), &payload, sizeof(payload)) == 0);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(zero_size_record) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            auto hdr = chan.producerReserve(0u);
            PPR_TEST_ASSERT(hdr.has_value());
            PPR_TEST_ASSERT(hdr->size() == 0u);
            chan.producerSubmit(*hdr);

            const auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(read->size() == 0u);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(consumer_blocks_until_data) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            std::jthread producer([&chan] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto hdr = chan.producerReserve(sizeof(int));
                PPR_TEST_ASSERT(hdr.has_value());
                *static_cast<int *>(hdr->data()) = 12345;
                chan.producerSubmit(*hdr);
            });

            auto read = chan.consumerAcquire();
            PPR_TEST_ASSERT(read.has_value());
            PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 12345);
            chan.consumerRelease(*read);
        };

        PPR_UNIT_TEST(concurrent_spsc) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            constexpr int num_messages = 1000;

            std::jthread producer([&chan] {
                for (int i = 0; i < num_messages; ++i) {
                    auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
                    PPR_TEST_ASSERT(hdr.has_value());
                    *static_cast<int *>(hdr->data()) = i;
                    chan.producerSubmit(*hdr);
                }
            });

            for (int i = 0; i < num_messages; ++i) {
                auto hdr = chan.consumerAcquire();
                PPR_TEST_ASSERT(hdr.has_value());
                PPR_TEST_ASSERT(*static_cast<int *>(hdr->data()) == i);
                chan.consumerRelease(*hdr);
            }

            PPR_TEST_ASSERT(chan.close().has_value());
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
                    PPR_TEST_ASSERT(close.has_value());
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
                        PPR_TEST_ASSERT(hdr.has_value());

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
                    PPR_TEST_ASSERT(false && "branch should not be reachable");
                }
            }

            for (auto &p: producers) {
                p.join();
            }

            constexpr int total = num_producers * messages_per_thread;
            PPR_TEST_ASSERT(received == total);
            const int local_seed_send = seed_send.load();
            PPR_TEST_ASSERT(local_seed_send == seed_recv);
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
                        PPR_TEST_ASSERT(hdr.has_value());
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
                            PPR_TEST_ASSERT(false && "branch should not be reachable");
                        }
                    }
                });
            }

            for (auto &p: producers) {
                p.join();
            }

            for (RawChannel &chan: channels) {
                const auto flush = chan.flush();
                PPR_TEST_ASSERT(flush.has_value());
            }

            constexpr int total = num_producers * messages_per_thread;
            PPR_TEST_ASSERT(received.load() == total);
            PPR_TEST_ASSERT(seed_send.load() == seed_recv.load());

            for (RawChannel &chan: channels) {
                const auto close = chan.close();
                PPR_TEST_ASSERT(close.has_value());
            }

            for (auto &p: consumers) {
                p.join();
            }
        };

        PPR_UNIT_TEST(concurrent_close_wakeup) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            std::jthread consumer([&chan] {
                const auto hdr = chan.consumerAcquire();
                PPR_TEST_ASSERT(not hdr.has_value());
                PPR_TEST_ASSERT(hdr.error() == RawChannel::error_closed);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto close = chan.close();
            PPR_TEST_ASSERT(close.has_value());
        };

        PPR_UNIT_TEST(select_one) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            const auto send = chan.producerReserve(8, RawChannel::wait_if_full);
            PPR_TEST_ASSERT(send.has_value());
            chan.producerSubmit(*send);

            auto signal = select(chan);

            const auto event = signal.poll();
            PPR_TEST_ASSERT(event.has_value());

            const auto recv = event.value()->consumerAcquire(RawChannel::block_until_available);
            PPR_TEST_ASSERT(recv.has_value());
            event.value()->consumerRelease(*recv);

            signal.reset();
        };

        PPR_UNIT_TEST(select_multiple) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};

            const auto send = chan_a.producerReserve(8, RawChannel::wait_if_full);
            PPR_TEST_ASSERT(send.has_value());
            chan_a.producerSubmit(*send);

            auto signal = select(chan_a, chan_b);

            auto event = signal.poll();
            PPR_TEST_ASSERT(event.has_value());
            PPR_TEST_ASSERT(event->index() == 0u);

            const auto recv = chan_a.consumerAcquire(RawChannel::block_until_available);
            PPR_TEST_ASSERT(recv.has_value());
            chan_a.consumerRelease(*recv);

            signal.reset(*event);
        };

        PPR_UNIT_TEST(select_close) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};

            PPR_TEST_ASSERT(chan_a.close().has_value());
            PPR_TEST_ASSERT(chan_b.close().has_value());

            auto signal = select(chan_a, chan_b);

            auto event = signal.poll();
            PPR_TEST_ASSERT(event.has_value());

            std::visit([](RawChannel *p_chan) {
                const auto recv = p_chan->consumerAcquire(RawChannel::block_until_available);
                PPR_TEST_ASSERT(not recv.has_value());
                PPR_TEST_ASSERT(recv.error() == RawChannel::error_closed);
            }, event.value());

            signal.reset(*event);
        };

        PPR_UNIT_TEST(select_loop) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};

            PPR_TEST_ASSERT(chan_a.close().has_value());
            PPR_TEST_ASSERT(chan_b.close().has_value());

            bool chan_a_closed = false;
            bool chan_b_closed = false;

            for (auto event: select(chan_a, chan_b)) {
                std::visit([&](RawChannel *p_chan) {
                    const auto recv = p_chan->consumerAcquire(RawChannel::block_until_available);
                    PPR_TEST_ASSERT(not recv.has_value());
                    PPR_TEST_ASSERT(recv.error() == RawChannel::error_closed);
                    if (p_chan == &chan_a) {
                        chan_a_closed = true;
                    } else if (p_chan == &chan_b) {
                        chan_b_closed = true;
                    }
                }, event);

                if (chan_a_closed && chan_b_closed) {
                    break;
                }
            }
        };

        // ------------------------------------------------------------------
        // Advanced select() tests
        // ------------------------------------------------------------------

        PPR_UNIT_TEST(select_same_channel_two_messages) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            // Submit first message
            auto send = chan.producerReserve(8, RawChannel::wait_if_full);
            PPR_TEST_ASSERT(send.has_value());
            chan.producerSubmit(*send);

            auto signal = select(chan);

            // First poll detects the first message
            {
                auto event = signal.poll();
                PPR_TEST_ASSERT(event.has_value());
                PPR_TEST_ASSERT(*event == std::addressof(chan));

                const auto recv = (*event)->consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(recv.has_value());
                (*event)->consumerRelease(*recv);
                signal.reset();
            }

            // No more pending after reset
            PPR_TEST_ASSERT(not signal.poll().has_value());

            // Submit second message
            send = chan.producerReserve(8, RawChannel::wait_if_full);
            PPR_TEST_ASSERT(send.has_value());
            chan.producerSubmit(*send);

            // Second poll detects the new message (PulseEvent re-triggered after reset)
            {
                auto event = signal.poll();
                PPR_TEST_ASSERT(event.has_value());
                PPR_TEST_ASSERT(*event == std::addressof(chan));

                const auto recv = (*event)->consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(recv.has_value());
                (*event)->consumerRelease(*recv);
                signal.reset();
            }

            PPR_TEST_ASSERT(not signal.poll().has_value());
        };

        PPR_UNIT_TEST(select_notify_before_subscribe) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};

            // Submit BEFORE creating the Signal
            auto send = chan_a.producerReserve(8, RawChannel::wait_if_full);
            PPR_TEST_ASSERT(send.has_value());
            chan_a.producerSubmit(*send);

            // Signal subscribes — should detect the already-fired PulseEvent
            // via subscribeEvent → emitEvent → notify path
            auto signal = select(chan_a, chan_b);

            auto event = signal.poll();
            PPR_TEST_ASSERT(event.has_value());
            PPR_TEST_ASSERT(event->index() == 0u);
            PPR_TEST_ASSERT(std::get<0u>(*event) == std::addressof(chan_a));

            const auto recv = chan_a.consumerAcquire(RawChannel::peek_without_blocking);
            PPR_TEST_ASSERT(recv.has_value());
            chan_a.consumerRelease(*recv);
            signal.reset(*event);

            PPR_TEST_ASSERT(not signal.poll().has_value());
        };

        PPR_UNIT_TEST(select_all_channels_ready) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_c{static_cast<std::size_t>(hal::page_granularity)};

            // Submit data to all three channels before select
            for (auto *chan : {&chan_a, &chan_b, &chan_c}) {
                auto send = chan->producerReserve(8, RawChannel::wait_if_full);
                PPR_TEST_ASSERT(send.has_value());
                *static_cast<int *>(send->data()) = 42;
                chan->producerSubmit(*send);
            }

            // Range-for iterates through all three ready channels
            std::size_t seen = 0;
            for (auto event : select(chan_a, chan_b, chan_c)) {
                std::visit([&](RawChannel *p_chan) {
                    const auto recv = p_chan->consumerAcquire(RawChannel::peek_without_blocking);
                    PPR_TEST_ASSERT(recv.has_value());
                    PPR_TEST_ASSERT(*static_cast<const int *>(recv->data()) == 42);
                    p_chan->consumerRelease(*recv);
                    ++seen;
                }, event);

                if (seen == 3u) {
                    break;
                }
            }

            PPR_TEST_ASSERT(seen == 3u);
        };

        PPR_UNIT_TEST(select_all_closed_range_for) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_c{static_cast<std::size_t>(hal::page_granularity)};

            PPR_TEST_ASSERT(chan_a.close().has_value());
            PPR_TEST_ASSERT(chan_b.close().has_value());
            PPR_TEST_ASSERT(chan_c.close().has_value());

            std::size_t closed_count = 0;
            for (auto event : select(chan_a, chan_b, chan_c)) {
                std::visit([&](RawChannel *p_chan) {
                    const auto recv = p_chan->consumerAcquire(RawChannel::block_until_available);
                    PPR_TEST_ASSERT(not recv.has_value());
                    PPR_TEST_ASSERT(recv.error() == RawChannel::error_closed);
                    ++closed_count;
                }, event);

                if (closed_count == 3u) {
                    break;
                }
            }

            PPR_TEST_ASSERT(closed_count == 3u);
        };

        PPR_UNIT_TEST(select_three_channels_mixed) {
            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_c{static_cast<std::size_t>(hal::page_granularity)};

            // Chan A: data, Chan B: closed, Chan C: data
            {
                auto send = chan_a.producerReserve(8, RawChannel::wait_if_full);
                PPR_TEST_ASSERT(send.has_value());
                *static_cast<int *>(send->data()) = 100;
                chan_a.producerSubmit(*send);
            }

            PPR_TEST_ASSERT(chan_b.close().has_value());

            {
                auto send = chan_c.producerReserve(8, RawChannel::wait_if_full);
                PPR_TEST_ASSERT(send.has_value());
                *static_cast<int *>(send->data()) = 300;
                chan_c.producerSubmit(*send);
            }

            std::size_t data_received = 0;
            bool closed_detected = false;

            for (auto event : select(chan_a, chan_b, chan_c)) {
                std::visit([&](RawChannel *p_chan) {
                    const auto recv = p_chan->consumerAcquire(RawChannel::peek_without_blocking);
                    if (recv.has_value()) {
                        const int val = *static_cast<const int *>(recv->data());
                        PPR_TEST_ASSERT(val == 100 || val == 300);
                        p_chan->consumerRelease(*recv);
                        ++data_received;
                    } else {
                        PPR_TEST_ASSERT(recv.error() == RawChannel::error_closed);
                        closed_detected = true;
                    }
                }, event);

                if (data_received == 2u && closed_detected) {
                    break;
                }
            }

            PPR_TEST_ASSERT(data_received == 2u);
            PPR_TEST_ASSERT(closed_detected);
        };

        PPR_UNIT_TEST(select_close_wakes_waiting_thread) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

            std::atomic<bool> woke{false};

            std::jthread consumer([&chan, &woke] {
                auto signal = select(chan);
                signal.wait();
                woke.store(true, std::memory_order_release);

                const auto event = signal.poll();
                PPR_TEST_ASSERT(event.has_value());

                const auto recv = event.value()->consumerAcquire(RawChannel::block_until_available);
                PPR_TEST_ASSERT(not recv.has_value());
                PPR_TEST_ASSERT(recv.error() == RawChannel::error_closed);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            PPR_TEST_ASSERT(not woke.load(std::memory_order_acquire));

            PPR_TEST_ASSERT(chan.close().has_value());

            consumer.join();
            PPR_TEST_ASSERT(woke.load(std::memory_order_acquire));
        };

        PPR_UNIT_TEST(select_concurrent_wakeup_and_drain) {
            RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
            constexpr int num_messages = 500;

            std::jthread producer([&chan] {
                for (int i = 0; i < num_messages; ++i) {
                    auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
                    PPR_TEST_ASSERT(hdr.has_value());
                    *static_cast<int *>(hdr->data()) = i;
                    chan.producerSubmit(*hdr);
                }
            });

            std::size_t received = 0;

            // Use range-for with select; drain all available data on each wakeup
            for (auto &event : select(chan)) {
                // Drain ALL messages in the channel
                while (true) {
                    const auto recv = event.consumerAcquire(RawChannel::peek_without_blocking);
                    if (not recv.has_value()) {
                        break;
                    }
                    PPR_TEST_ASSERT(*static_cast<const int *>(recv->data()) == static_cast<int>(received));
                    event.consumerRelease(*recv);
                    ++received;
                }

                if (received >= static_cast<std::size_t>(num_messages)) {
                    break;
                }
            }

            producer.join();
            PPR_TEST_ASSERT(received == static_cast<std::size_t>(num_messages));
        };

        PPR_UNIT_TEST(select_multiple_channels_concurrent) {
            constexpr int messages_per_producer = 200;
            constexpr int num_channels = 3;

            RawChannel chan_a{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_b{static_cast<std::size_t>(hal::page_granularity)};
            RawChannel chan_c{static_cast<std::size_t>(hal::page_granularity)};

            std::atomic<int> seed_send{0};

            auto producer_fn = [&](RawChannel &chan) {
                for (int i = 0; i < messages_per_producer; ++i) {
                    auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
                    PPR_TEST_ASSERT(hdr.has_value());
                    *static_cast<int *>(hdr->data()) = i;
                    seed_send += i;
                    chan.producerSubmit(*hdr);
                }
            };

            std::jthread prod_a(producer_fn, std::ref(chan_a));
            std::jthread prod_b(producer_fn, std::ref(chan_b));
            std::jthread prod_c(producer_fn, std::ref(chan_c));

            // Consumer: use select with wait/poll, draining all messages on wakeup
            int received = 0;
            int seed_recv = 0;

            for (auto &event : select(chan_a, chan_b, chan_c)) {
                // Drain ALL messages from the ready channel
                RawChannel *const p_chan = std::visit([](RawChannel *p) { return p; }, event);
                while (true) {
                    const auto recv = p_chan->consumerAcquire(RawChannel::peek_without_blocking);
                    if (not recv.has_value()) {
                        break;
                    }
                    seed_recv += *static_cast<const int *>(recv->data());
                    p_chan->consumerRelease(*recv);
                    ++received;
                }

                if (received >= messages_per_producer * num_channels) {
                    break;
                }
            }

            prod_a.join();
            prod_b.join();
            prod_c.join();

            PPR_TEST_ASSERT(received == messages_per_producer * num_channels);
            PPR_TEST_ASSERT(static_cast<int>(seed_send.load()) == seed_recv);
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

            PPR_TEST_ASSERT(chan.emplace(42).has_value());
            const auto result = chan.peek();
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(*result == 42);
        };

        PPR_UNIT_TEST(emplace_construction) {
            auto chan = pP::Channel<std::string>(std::in_place_t{}, 16u);

            const auto sent = chan.send(std::string("hello world"));
            PPR_TEST_ASSERT(sent.has_value());

            const auto result = chan.peek();
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(result.value() == "hello world");
        };

        PPR_UNIT_TEST(peek_non_blocking) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            const auto empty = chan.peek();
            PPR_TEST_ASSERT(!empty.has_value());

            const auto sent = chan.send(99);
            PPR_TEST_ASSERT(sent.has_value());

            const auto val = chan.peek();
            PPR_TEST_ASSERT(val.has_value());
            PPR_TEST_ASSERT(val.value() == 99);
        };

        PPR_UNIT_TEST(send_receive_string) {
            auto chan = pP::Channel<std::string>(std::in_place_t{}, 8u);

            std::string msg = "test message";
            PPR_TEST_ASSERT(chan.emplace(std::move(msg)).has_value());
            PPR_TEST_ASSERT(msg.empty());

            const auto result = chan.peek();
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(result.value() == "test message");
        };

        PPR_UNIT_TEST(operator_stream_send) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            auto err = chan << 10 << 20 << 30;
            PPR_TEST_ASSERT(not err.has_error());

            const auto r1 = chan.peek();
            PPR_TEST_ASSERT(r1.value() == 10);
            const auto r2 = chan.peek();
            PPR_TEST_ASSERT(r2.value() == 20);
            const auto r3 = chan.peek();
            PPR_TEST_ASSERT(r3.value() == 30);
        };

        PPR_UNIT_TEST(operator_stream_receive) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            auto sent = chan.send(42);
            PPR_TEST_ASSERT(sent.has_value());

            sent = chan.send(84);
            PPR_TEST_ASSERT(sent.has_value());

            int val = 0;
            PPR_TEST_ASSERT((chan >> val).has_value());
            PPR_TEST_ASSERT(val == 42);
            PPR_TEST_ASSERT((chan >> val).has_value());
            PPR_TEST_ASSERT(val == 84);
        };

        PPR_UNIT_TEST(in_place_construction) {
            pP::Channel<int> chan{std::in_place_t{}, 32u};

            auto sent = chan.send(1);
            PPR_TEST_ASSERT(sent.has_value());
            sent = chan.send(2);
            PPR_TEST_ASSERT(sent.has_value());
            sent = chan.send(3);
            PPR_TEST_ASSERT(sent.has_value());

            PPR_TEST_ASSERT(chan.receive().value() == 1);
            PPR_TEST_ASSERT(chan.receive().value() == 2);
            PPR_TEST_ASSERT(chan.receive().value() == 3);
        };

        PPR_UNIT_TEST(close_propagation) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_TEST_ASSERT(chan.send(1).has_value());

            const auto close = chan.close();
            PPR_TEST_ASSERT(close.has_value());

            const auto recv = chan.receive();
            PPR_TEST_ASSERT(recv.has_value());
            PPR_TEST_ASSERT(recv.value() == 1);

            const auto peak = chan.peek();
            PPR_TEST_ASSERT(not peak.has_value());

            PPR_TEST_ASSERT(not chan.send(2));
        };

        PPR_UNIT_TEST(backpressure_drop) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 2u);

            int sent = 0;
            for (int i = 1; chan.send(i); i++) {
                sent += i;
            }

            int received = 0;
            while (auto opt = chan.peek()) {
                PPR_TEST_ASSERT(opt.has_value());
                received += opt.value();
            }

            PPR_TEST_ASSERT(sent == received);
        };

        PPR_UNIT_TEST(destructor_called_on_consume) {
            details::TrackedDestruction::count = 0u;

            auto chan = pP::Channel<details::TrackedDestruction>(std::in_place_t{}, 8u);
            PPR_TEST_ASSERT(chan.emplace(42).has_value());

            const auto result = chan.receive();
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(result.value().value == 42);

            const std::size_t n = details::TrackedDestruction::count;
            PPR_TEST_ASSERT(n == 1u);

            const auto close = chan.close();
            PPR_TEST_ASSERT(close.has_value());
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
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(result.value() == i);
            }

            PPR_TEST_ASSERT(chan.close().has_value());
        };

        PPR_UNIT_TEST(operator_receive_on_closed) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);
            PPR_TEST_ASSERT(chan.close().has_value());

            int val = 0;
            PPR_TEST_ASSERT(not (chan >> val));
        };

        PPR_UNIT_TEST(operator_receive_empty) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            int count = 0;
            for (auto it = chan.begin(RawChannel::peek_without_blocking); it != chan.end(); ++it) {
                ++count;
            }
            PPR_TEST_ASSERT(count == 0);
        };

        PPR_UNIT_TEST(shared_ptr_construction) {
            auto raw = std::make_shared<RawChannel>(static_cast<std::size_t>(hal::page_granularity));
            auto chan = pP::Channel<int>(std::move(raw));

            const auto sent = chan.send(777);
            PPR_TEST_ASSERT(sent.has_value());

            const auto result = chan.receive();
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(result.value() == 777);
        };

        PPR_UNIT_TEST(flush) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_TEST_ASSERT(chan.send(1).has_value());
            PPR_TEST_ASSERT(chan.send(2).has_value());

            std::atomic<int> total{0};

            std::jthread consumer([&] {
                const auto r1 = chan.receive();
                PPR_TEST_ASSERT(r1.value() == 1);
                total += r1.value();
                const auto r2 = chan.receive();
                PPR_TEST_ASSERT(r2.value() == 2);
                total += r2.value();
                const auto r3 = chan.receive();
                PPR_TEST_ASSERT(not r3);
            });

            PPR_TEST_ASSERT(!!chan.flush());
            PPR_TEST_ASSERT(total == 3);
            PPR_TEST_ASSERT(!!chan.close());
        };

        PPR_UNIT_TEST(auto_close_in_destructor) {
            {
                const RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
                PPR_TEST_ASSERT(chan.isOpened());
            }
        };

        PPR_UNIT_TEST(channel_destructor_drains_non_trivial) {
            details::TrackedDestruction::count = 0u;
            {
                auto chan = pP::Channel<details::TrackedDestruction>(std::in_place_t{}, 8u);
                PPR_TEST_ASSERT(chan.emplace(42).has_value());
                PPR_TEST_ASSERT(chan.emplace(99).has_value());
                const auto r1 = chan.receive();
                PPR_TEST_ASSERT(r1.has_value());
                PPR_TEST_ASSERT(r1->value == 42);
                const auto r2 = chan.receive();
                PPR_TEST_ASSERT(r2.has_value());
                PPR_TEST_ASSERT(r2->value == 99);
                const std::size_t n = details::TrackedDestruction::count;
                PPR_TEST_ASSERT(n == 2u);
            }
            const std::size_t n = details::TrackedDestruction::count;
            PPR_TEST_ASSERT(n == 4u);
        };

        PPR_UNIT_TEST(range_iteration_blocking) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            std::jthread producer([&chan] {
                for (int i = 0; i < 5; ++i) {
                    PPR_TEST_ASSERT(chan.emplace(i).has_value());
                }
                PPR_TEST_ASSERT(chan.close().has_value());
            });

            int expected = 0;
            for (const auto &msg: chan) {
                PPR_TEST_ASSERT(msg == expected++);
            }
            PPR_TEST_ASSERT(expected == 5);
        };

        PPR_UNIT_TEST(range_iteration_non_blocking) {
            auto chan = pP::Channel<int>(std::in_place_t{}, 16u);

            PPR_TEST_ASSERT(chan.emplace(1).has_value());
            PPR_TEST_ASSERT(chan.emplace(2).has_value());
            PPR_TEST_ASSERT(chan.emplace(3).has_value());
            PPR_TEST_ASSERT(chan.close().has_value());

            int sum = 0;
            for (auto it = chan.begin(RawChannel::EPolling::peek_without_blocking); it != chan.end(); ++it) {
                sum += *it;
            }
            PPR_TEST_ASSERT(sum == 6);
        };
    }

    PPR_UNIT_TEST(raw_channel) {
        _.recurse({
            ChannelRaw::construction_and_state,
            ChannelRaw::single_threaded_send_receive,
            ChannelRaw::multiple_messages_sequence,
            ChannelRaw::ring_buffer_wrap_around,
            ChannelRaw::backpressure_drop_if_full,
            ChannelRaw::backpressure_yield_if_full,
            ChannelRaw::backpressure_wait_if_full,
            ChannelRaw::discard_record,
            ChannelRaw::flush_roundtrip,
            ChannelRaw::peek_without_blocking_empty,
            ChannelRaw::peek_without_blocking_with_data,
            ChannelRaw::close_idempotent,
            ChannelRaw::close_record_consumed,
            ChannelRaw::record_header_accessors,
            ChannelRaw::zero_size_record,
            ChannelRaw::consumer_blocks_until_data,
            ChannelRaw::concurrent_spsc,
            ChannelRaw::concurrent_mpsc,
            ChannelRaw::concurrent_mpmc,
            ChannelRaw::concurrent_close_wakeup,
            ChannelRaw::select_one,
            ChannelRaw::select_multiple,
            ChannelRaw::select_close,
            ChannelRaw::select_loop,
            ChannelRaw::select_same_channel_two_messages,
            ChannelRaw::select_notify_before_subscribe,
            ChannelRaw::select_all_channels_ready,
            ChannelRaw::select_all_closed_range_for,
            ChannelRaw::select_three_channels_mixed,
            ChannelRaw::select_close_wakes_waiting_thread,
            ChannelRaw::select_concurrent_wakeup_and_drain,
            ChannelRaw::select_multiple_channels_concurrent,
        });
    };

    PPR_UNIT_TEST(typed_channel) {
        _.recurse({
            ChannelTyped::send_and_receive_int,
            ChannelTyped::emplace_construction,
            ChannelTyped::peek_non_blocking,
            ChannelTyped::send_receive_string,
            ChannelTyped::operator_stream_send,
            ChannelTyped::operator_stream_receive,
            ChannelTyped::in_place_construction,
            ChannelTyped::close_propagation,
            ChannelTyped::backpressure_drop,
            ChannelTyped::destructor_called_on_consume,
            ChannelTyped::concurrent_channel,
            ChannelTyped::operator_receive_on_closed,
            ChannelTyped::operator_receive_empty,
            ChannelTyped::shared_ptr_construction,
            ChannelTyped::flush,
            ChannelTyped::auto_close_in_destructor,
            ChannelTyped::channel_destructor_drains_non_trivial,
            ChannelTyped::range_iteration_blocking,
            ChannelTyped::range_iteration_non_blocking,
        });
    };

    PPR_UNIT_TEST(channel) {
        _.recurse({
            raw_channel,
            typed_channel,
        });
    };
}
