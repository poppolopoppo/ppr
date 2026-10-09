module engine.app;

import :startup.settings;

import std;

namespace pP {
    [[nodiscard]] Array<config::OptionDecl> startupCliOptions() {
        Array<config::OptionDecl> options{};

        config::OptionDecl core{};
        core.m_canonical = "startup.core_validation";
        core.m_description = "Enable RHI core validation layers.";
        core.m_aliases.push_back("core-validation");
        core.m_is_switch = true;
        options.push_back(std::move(core));

        config::OptionDecl gpu{};
        gpu.m_canonical = "startup.gpu_validation";
        gpu.m_description = "Enable GPU-assisted validation (default off everywhere).";
        gpu.m_aliases.push_back("gpu-validation");
        gpu.m_is_switch = true;
        options.push_back(std::move(gpu));

        return options;
    }
} // namespace pP
