module;

#include <unistd.h>
#include <sys/wait.h>
#include <crt_externs.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::process {
    [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false) {
        uint32_t buf_size = 0;
        ::_NSGetExecutablePath(nullptr, &buf_size);

        std::string path_buf(buf_size, '\0');
        if (::_NSGetExecutablePath(path_buf.data(), &buf_size) != 0) {
            throw std::runtime_error("Failed to get executable path");
        }

        return std::filesystem::path(std::move(path_buf));
    }

    [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            std::vector<const char *> argv;
            argv.reserve(args.size() + 2);
            argv.push_back(executable.c_str());
            for (const auto &arg : args) {
                argv.push_back(arg.c_str());
            }
            argv.push_back(nullptr);

            ::execvp(executable.c_str(), const_cast<char *const *>(argv.data()));
            ::_exit(127);
        }

        if (pid < 0) {
            throw std::runtime_error("fork failed");
        }

        int status = 0;
        ::waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }
}
