module;

#include <GLFW/glfw3.h>

#include "pP/Macros.h"

module engine.app;

import std;
import engine.core;
import engine.rhi;

namespace app {

    Application::Application(const std::string_view name, const std::span<const char *const> argv)
        : m_arguments(argv.begin(), argv.end())
        , m_name(name) {

    }

    Application::~Application() noexcept = default;

    int Application::run() {
        setExitCode(0);

        if (initialize()) [[likely]] {
            PPR_DEFER{ terminate(); };

            while (update()) [[likely]] {
                render();
            }
        }

        return m_exitCode;
    }

    bool Application::initialize() {
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW\n";
            setExitCode(-2);
            return false;
        }
        std::cout << "engine.app initialized (GLFW " << glfwGetVersionString() << ")\n";

        rhi::ComPtr<rhi::IDevice> device;
        return rhi::initialize();
    }

    bool Application::update() {
        glfwPollEvents();
        //return glfwWindowShouldClose(nullptr);
        return true;
    }

    void Application::render() {

    }

    void Application::terminate() {
        glfwTerminate();
        std::cout << "engine.app terminated.\n";
    }

}
