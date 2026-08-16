module;
#include "pP/UnitTest.h"

export module engine.tests.core:io.file_watcher;

import engine.core;

import std;

export namespace pP::tests {

    namespace FileWatcherTests {

        struct TempDir {
            std::filesystem::path path;
            TempDir() {
                auto base = std::filesystem::temp_directory_path() / "ppr_fw_test";
                base /= std::to_string(std::random_device{}());
                std::filesystem::create_directories(base);
                path = std::move(base);
            }
            ~TempDir() { std::filesystem::remove_all(path); }
            TempDir(const TempDir &) = delete;
            TempDir(TempDir &&) = delete;
            TempDir &operator=(const TempDir &) = delete;
            TempDir &operator=(TempDir &&) = delete;
        };

        [[nodiscard]] bool pollUntilChanges(DirectoryWatcher &w, const int max_attempts = 50, const int sleep_ms = 20) {
            std::error_code ec;
            for (int i = 0; i < max_attempts; ++i) {
                w.poll(ec);
                if (ec && ec != std::errc::result_out_of_range) return false;
                if (not w.changes().empty()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
            return false;
        }

        PPR_UNIT_TEST(create_file) {
            TempDir td;
            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);

            { std::ofstream ofs(td.path / "test.txt"); ofs << "hello"; }

            PPR_TEST_ASSERT(pollUntilChanges(w));
            const auto c = w.changes();
            PPR_TEST_ASSERT(c.size() > 0u);
            const auto act = c[0].m_action;
            PPR_TEST_ASSERT(act == WatchEvent::Action::added || act == WatchEvent::Action::modified);
        };

        PPR_UNIT_TEST(modify_file) {
            TempDir td;
            {
                std::ofstream ofs(td.path / "data.bin", std::ios::binary);
                ofs.put(0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);

            {
                std::ofstream ofs(td.path / "data.bin", std::ios::binary | std::ios::app);
                ofs.put(42);
            }

            PPR_TEST_ASSERT(pollUntilChanges(w));
            const auto c = w.changes();
            PPR_TEST_ASSERT(c.size() > 0u);
            PPR_TEST_ASSERT(c[0].m_action == WatchEvent::Action::modified);
        };

        PPR_UNIT_TEST(delete_file) {
            TempDir td;
            { std::ofstream ofs(td.path / "todelete.txt"); ofs << "x"; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);

            std::filesystem::remove(td.path / "todelete.txt");

            PPR_TEST_ASSERT(pollUntilChanges(w));
            const auto c = w.changes();
            PPR_TEST_ASSERT(c.size() > 0u);
            PPR_TEST_ASSERT(c[0].m_action == WatchEvent::Action::removed);
        };

        PPR_UNIT_TEST(poll_idempotent_no_changes) {
            TempDir td;
            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);
            PPR_TEST_ASSERT(w.changes().empty());

            w.poll(ec);
            PPR_TEST_ASSERT(not ec);
            PPR_TEST_ASSERT(w.changes().empty());
        };

        PPR_UNIT_TEST(changes_cached_until_next_poll) {
            TempDir td;
            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);
            PPR_TEST_ASSERT(w.changes().empty());

            { std::ofstream ofs(td.path / "cached.txt"); ofs << "x"; }
            PPR_TEST_ASSERT(pollUntilChanges(w));

            const auto c1 = w.changes();
            PPR_TEST_ASSERT(not c1.empty());

            const auto c2 = w.changes();
            PPR_TEST_ASSERT(c2.size() == c1.size());
        };

        PPR_UNIT_TEST(i_event_interface) {
            TempDir td;
            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);
            PPR_TEST_ASSERT(not w.pollEvent());

            { std::ofstream ofs(td.path / "event_test.txt"); ofs << "x"; }
            PPR_TEST_ASSERT(pollUntilChanges(w));
            PPR_TEST_ASSERT(not w.changes().empty());

            w.resetEvent();
            PPR_TEST_ASSERT(w.changes().empty());
        };

        PPR_UNIT_TEST(multiple_files) {
            TempDir td;
            DirectoryWatcher w(td.path);
            std::error_code ec;
            w.poll(ec);
            PPR_TEST_ASSERT(not ec);

            { std::ofstream ofs(td.path / "a.txt"); ofs << "a"; }
            { std::ofstream ofs(td.path / "b.txt"); ofs << "b"; }
            { std::ofstream ofs(td.path / "c.txt"); ofs << "c"; }

            PPR_TEST_ASSERT(pollUntilChanges(w));
            const auto c = w.changes();
            PPR_TEST_ASSERT(not c.empty());
        };

    }

    PPR_UNIT_TEST(file_watcher) {
        _.recurse(FileWatcherTests::create_file);
        _.recurse(FileWatcherTests::modify_file);
        _.recurse(FileWatcherTests::delete_file);
        _.recurse(FileWatcherTests::poll_idempotent_no_changes);
        _.recurse(FileWatcherTests::changes_cached_until_next_poll);
        _.recurse(FileWatcherTests::i_event_interface);
        _.recurse(FileWatcherTests::multiple_files);
    };
}
