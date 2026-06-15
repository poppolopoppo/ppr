module;
#include "pP/Macros.h"
export module engine.tests:core_io;
import engine.core;
import std;

export namespace pP::tests {

    namespace IoTests {

        namespace File {

            PPR_UNIT_TEST(open_and_close) {
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_open.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write("test", 4);
                }

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;
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

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto file = std::move(*file_result);
                PPR_ASSERT(file.isValid());

                auto file2 = std::move(file);
                PPR_ASSERT(not file.isValid());
                PPR_ASSERT(file2.isValid());
            };

            PPR_UNIT_TEST(default_constructed_invalid) {
                IoFile file;
                PPR_ASSERT(not file.isValid());
            };

            PPR_UNIT_TEST(double_close_safe) {
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_dclose.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write("safe", 4);
                }

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;
                PPR_ASSERT(file.isValid());
                file.close();
                file.close();
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

                auto mapped_result = io::mapFile(path);
                PPR_ASSERT(mapped_result.has_value());
                auto &mapped = *mapped_result;
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

                auto mapped_result = io::mapFile(path);
                PPR_ASSERT(mapped_result.has_value());
                auto &mapped = *mapped_result;
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

                auto mapped_result = io::mapFile(path);
                PPR_ASSERT(mapped_result.has_value());
                auto mapped = std::move(*mapped_result);
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

            PPR_UNIT_TEST(write_content) {
                constexpr std::string_view kInitial = "Hello, World!";
                constexpr std::string_view kWrite  = "MappedWrite";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_mmap_write.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kInitial.data(), static_cast<std::streamsize>(kInitial.size()));
                }

                auto mapped_result = io::mapFile(path, hal::io::OpenFlags{hal::io::OpenFlags::read | hal::io::OpenFlags::write});
                PPR_ASSERT(mapped_result.has_value());
                auto &mapped = *mapped_result;
                PPR_ASSERT(mapped.isValid());
                PPR_ASSERT(mapped.size() == kInitial.size());

                {
                    auto sp = mapped.span();
                    PPR_ASSERT(not sp.empty());
                    std::memcpy(sp.data(), kWrite.data(), kWrite.size());
                }

                {
                    const auto &cm = mapped;
                    const auto sp = cm.span();
                    PPR_ASSERT(std::memcmp(sp.data(), kWrite.data(), kWrite.size()) == 0);
                    PPR_ASSERT(sp[kInitial.size() - 1u] == std::byte{'!'});
                }
            };

        }

        namespace Request {

            PPR_UNIT_TEST(default_state) {
                auto port = io::createPort();
                IoRequest req;
                PPR_ASSERT(not req.isPending());
                PPR_ASSERT(not req.pollEvent());
                PPR_ASSERT(not req.cancel());
            };

            PPR_UNIT_TEST(i_event_interface) {
                auto port = io::createPort();
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

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;
                PPR_ASSERT(file.isValid());

                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                (void)port.pollCompletions();

                auto signal = select(req);
                const auto result = signal.poll();
                PPR_ASSERT(result.has_value());
                PPR_ASSERT(result.value() == std::addressof(req));

                PPR_ASSERT(req.bytesTransferred() == kContent.size());
                PPR_ASSERT(not req.error());
                PPR_ASSERT(std::memcmp(buf.data(), kContent.data(), kContent.size()) == 0);
            };

            PPR_UNIT_TEST(poll_idle_returns_zero) {
                auto port = io::createPort();
                const std::size_t n = port.pollCompletions();
                PPR_ASSERT(n == 0u);
            };

            PPR_UNIT_TEST(reset_and_reuse) {
                constexpr std::string_view kContent = "Reuse!";
                const auto path = std::filesystem::temp_directory_path() / "ppr_io_test_reuse.bin";
                PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
                {
                    std::ofstream ofs(path, std::ios::binary);
                    ofs.write(kContent.data(), static_cast<std::streamsize>(kContent.size()));
                }

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;

                IoRequest req;

                {
                    std::array<std::byte, 64> buf{};
                    port.read(req, file, buf, 0u);
                    (void)port.pollCompletions();
                    PPR_ASSERT(req.pollEvent());
                    PPR_ASSERT(req.bytesTransferred() == kContent.size());
                }

                req.resetEvent();

                {
                    std::array<std::byte, 64> buf{};
                    port.read(req, file, buf, 0u);
                    (void)port.pollCompletions();
                    PPR_ASSERT(req.pollEvent());
                    PPR_ASSERT(req.bytesTransferred() == kContent.size());
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

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;

                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                (void)port.pollCompletions();

                PulseEvent timer;
                auto signal = select(req, timer);
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

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;
                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                const bool was = req.cancel();
                PPR_ASSERT(not req.isPending());

                (void)port.pollCompletions();

                if (was) {
                    PPR_ASSERT(not req.pollEvent());
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

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;
                IoRequest req;
                std::array<std::byte, 64> buf{};
                port.read(req, file, buf, 0u);

                const bool first = req.cancel();
                (void)first;
                const bool second = req.cancel();
                PPR_ASSERT(not second);
                PPR_ASSERT(not req.isPending());

                (void)port.pollCompletions();
            };

            PPR_UNIT_TEST(cancel_i_event) {
                auto port = io::createPort();
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

                auto port = io::createPort();
                auto file_result = port.open(path);
                PPR_ASSERT(file_result.has_value());
                auto &file = *file_result;
                {
                    IoRequest req;
                    std::array<std::byte, 64> buf{};
                    port.read(req, file, buf, 0u);
                    (void)req.cancel();
                    (void)port.pollCompletions();
                }
                PPR_ASSERT(true);
            };

        }

        PPR_UNIT_TEST(file) {
            _.recurse({
                File::open_and_close,
                File::move_semantics,
                File::default_constructed_invalid,
                File::double_close_safe,
            });
        };

        PPR_UNIT_TEST(mapped) {
            _.recurse({
                Mapped::read_content,
                Mapped::empty_file,
                Mapped::move_semantics,
                Mapped::default_constructed_invalid,
                Mapped::write_content,
            });
        };

        PPR_UNIT_TEST(request) {
            _.recurse({
                Request::default_state,
                Request::i_event_interface,
                Request::poll_idle_returns_zero,
                Request::read_completes,
                Request::reset_and_reuse,
                Request::select_with_timer,
                Request::cancel_inflight,
                Request::cancel_idempotent,
                Request::cancel_i_event,
                Request::cancel_then_destroy,
            });
        };
    }

    PPR_UNIT_TEST(io) {
        _.recurse({
            IoTests::file,
            IoTests::mapped,
            IoTests::request,
        });
    };
}
