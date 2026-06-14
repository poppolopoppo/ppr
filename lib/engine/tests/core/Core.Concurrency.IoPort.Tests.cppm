module;
#include "pP/Macros.h"
export module engine.tests:core_io_port;
import engine.core;
import std;

export namespace pP::tests {

    namespace IoPortTests {

        PPR_UNIT_TEST(lifecycle) {
            constexpr std::string_view kContent = "Lifecycle!";
            const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_lifecycle.bin";
            PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
            {
                std::ofstream ofs(path, std::ios::binary);
                ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
            }

            IoPort port;
            auto file = port.open(path);
            PPR_ASSERT(file.isValid());
            file.close();
            PPR_ASSERT(not file.isValid());
        };

        namespace File {

            PPR_UNIT_TEST(open_and_close) {
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_open.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write("test", 4);
                }

                IoPort port;
                auto file = port.open(path);
                PPR_ASSERT(file.isValid());
                file.close();
                PPR_ASSERT(not file.isValid());
            };

            PPR_UNIT_TEST(move_semantics) {
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_move.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write("move", 4);
                }

                IoPort port;
                auto file = port.open(path);
                PPR_ASSERT(file.isValid());

                auto file2 = std::move(file);
                PPR_ASSERT(not file.isValid());
                PPR_ASSERT(file2.isValid());
            };

            PPR_UNIT_TEST(default_constructed_invalid) {
                IoFile file;
                PPR_ASSERT(not file.isValid());
            };

        }

        namespace Mapped {

            PPR_UNIT_TEST(read_content) {
                constexpr std::string_view kContent = "Hello, MappedFile!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_mmap.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                auto mapped = port.map(path);
                PPR_ASSERT(mapped.isValid());
                PPR_ASSERT(mapped.size() == kContent.size());

                const auto sp = mapped.span();
                PPR_ASSERT(sp.size() == kContent.size());
                PPR_ASSERT(std::memcmp(sp.data(), kContent.data(), kContent.size()) == 0);
            };

            PPR_UNIT_TEST(empty_file) {
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_empty.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                }

                IoPort port;
                auto mapped = port.map(path);
                PPR_ASSERT(mapped.isValid());
                PPR_ASSERT(mapped.size() == 0u);
                PPR_ASSERT(mapped.span().empty());
            };

            PPR_UNIT_TEST(move_semantics) {
                constexpr std::string_view kContent = "move";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_mmap_move.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                auto mapped = port.map(path);
                PPR_ASSERT(mapped.isValid());

                auto mapped2 = std::move(mapped);
                PPR_ASSERT(not mapped.isValid());
                PPR_ASSERT(mapped2.isValid());
                PPR_ASSERT(mapped2.size() == kContent.size());
            };

            PPR_UNIT_TEST(default_constructed_invalid) {
                MappedFile mapped;
                PPR_ASSERT(not mapped.isValid());
                PPR_ASSERT(mapped.size() == 0u);
                PPR_ASSERT(mapped.span().empty());
            };

        }

        namespace Request {

            PPR_UNIT_TEST(default_state) {
                IoPort port;
                IoRequest req;
                PPR_ASSERT(not req.isPending());
                PPR_ASSERT(not req.pollEvent());
                PPR_ASSERT(not req.cancel()); // idle → cancel returns false
            };

            PPR_UNIT_TEST(i_event_interface) {
                IoPort port;
                IoRequest req;

                PPR_ASSERT(not req.pollEvent());

                auto signal = select(req);
                PPR_ASSERT(not signal.poll().has_value());
            };

            PPR_UNIT_TEST(read_completes) {
                constexpr std::string_view kContent = "AsyncRead!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_read.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                IoFile file = port.open(path);
                PPR_ASSERT(file.isValid());

                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                auto signal = select(req);
                signal.wait();
                const auto result = signal.poll();
                PPR_ASSERT(result.has_value());
                PPR_ASSERT(result.value() == std::addressof(req));

                PPR_ASSERT(req.bytesTransferred() == kContent.size());
                PPR_ASSERT(not req.error());
                PPR_ASSERT(std::memcmp(buf.data(), kContent.data(), kContent.size()) == 0);
            };

            PPR_UNIT_TEST(reset_and_reuse) {
                constexpr std::string_view kContent = "Reuse!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_reuse.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                IoFile file = port.open(path);

                IoRequest req;

                {
                    std::array<std::byte, 64> buf{};
                    port.read(req, file, buf, 0u);

                    auto signal = select(req);
                    signal.wait();
                    const auto result = signal.poll();
                    PPR_ASSERT(result.has_value());
                    PPR_ASSERT(req.bytesTransferred() == kContent.size());
                    PPR_ASSERT(std::memcmp(buf.data(), kContent.data(), kContent.size()) == 0);
                }

                req.resetEvent();

                {
                    std::array<std::byte, 64> buf{};
                    port.read(req, file, buf, 0u);

                    auto signal = select(req);
                    signal.wait();
                    const auto result = signal.poll();
                    PPR_ASSERT(result.has_value());
                    PPR_ASSERT(req.bytesTransferred() == kContent.size());
                    PPR_ASSERT(std::memcmp(buf.data(), kContent.data(), kContent.size()) == 0);
                }
            };

            PPR_UNIT_TEST(select_with_timer) {
                constexpr std::string_view kContent = "TimerRead!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_timer.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                IoFile file = port.open(path);

                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                PulseEvent timer;
                auto signal = select(req, timer);
                signal.wait();
                const auto result = signal.poll();
                PPR_ASSERT(result.has_value());
                PPR_ASSERT(result->index() == 0u);
                PPR_ASSERT(req.bytesTransferred() == kContent.size());
            };

            PPR_UNIT_TEST(cancel_inflight) {
                constexpr std::string_view kContent = "CancelIo!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_cancel_inflight.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                IoFile file = port.open(path);
                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                const bool was_cancelled = req.cancel();
                PPR_ASSERT(not req.isPending());

                // Allow any racing completion notifications to drain to the poller thread
                for (int spin = 0; spin < 32; ++spin) {
                    std::this_thread::yield();
                }

                if (was_cancelled) {
                    PPR_ASSERT(not req.pollEvent());

                    IoRequest barrier;
                    std::array<std::byte, 64> barrier_buf{};
                    port.read(barrier, file, barrier_buf, 0u);
                    select(barrier).wait();
                } else {
                    PPR_ASSERT(req.bytesTransferred() == kContent.size());
                    req.resetEvent();
                }
            };

            PPR_UNIT_TEST(cancel_idempotent) {
                constexpr std::string_view kContent = "Idempotent!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_cancel_idem.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                IoFile file = port.open(path);
                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                const bool first = req.cancel();
                const bool second = req.cancel();
                PPR_ASSERT(not second);
                PPR_ASSERT(not req.isPending());

                // Allow any racing completion notifications to drain to the poller thread
                for (int spin = 0; spin < 32; ++spin) {
                    std::this_thread::yield();
                }

                if (first) {
                    IoRequest barrier;
                    std::array<std::byte, 64> barrier_buf{};
                    port.read(barrier, file, barrier_buf, 0u);
                    select(barrier).wait();
                }
            };

            PPR_UNIT_TEST(cancel_i_event) {
                IoPort port;
                IoRequest req;

                auto signal = select(req);
                PPR_ASSERT(not signal.poll().has_value());

                PPR_ASSERT(not req.cancel());
                PPR_ASSERT(not req.pollEvent());

                req.resetEvent();
                PPR_ASSERT(not req.pollEvent());

                auto signal2 = select(req);
                PPR_ASSERT(not signal2.poll().has_value());
            };

            PPR_UNIT_TEST(cancel_then_destroy) {
                constexpr std::string_view kContent = "Destroy!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_cancel_destroy.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                IoPort port;
                IoFile file = port.open(path);
                {
                    IoRequest req;
                    std::array<std::byte, 64> buf{};
                    port.read(req, file, buf, 0u);
                    (void)req.cancel();

                    // Allow any racing completion notifications to drain to the poller thread
                    for (int spin = 0; spin < 32; ++spin) {
                        std::this_thread::yield();
                    }

                    IoRequest barrier;
                    std::array<std::byte, 64> barrier_buf{};
                    port.read(barrier, file, barrier_buf, 0u);
                    select(barrier).wait();
                }
                PPR_ASSERT(true);
            };

        }

        PPR_UNIT_TEST(file) {
            _.recurse(File::open_and_close);
            _.recurse(File::move_semantics);
            _.recurse(File::default_constructed_invalid);
        };

        PPR_UNIT_TEST(mapped) {
            _.recurse(Mapped::read_content);
            _.recurse(Mapped::empty_file);
            _.recurse(Mapped::move_semantics);
            _.recurse(Mapped::default_constructed_invalid);
        };

        PPR_UNIT_TEST(request) {
            _.recurse(Request::default_state);
            _.recurse(Request::i_event_interface);
            _.recurse(Request::read_completes);
            _.recurse(Request::reset_and_reuse);
            _.recurse(Request::select_with_timer);
            _.recurse(Request::cancel_inflight);
            _.recurse(Request::cancel_idempotent);
            _.recurse(Request::cancel_i_event);
            _.recurse(Request::cancel_then_destroy);
        };
    }

    PPR_UNIT_TEST(ioPort) {
        _.recurse(IoPortTests::file);
        _.recurse(IoPortTests::mapped);
        _.recurse(IoPortTests::request);
    };
}
