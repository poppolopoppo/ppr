module;

#include "pP/Macros.h"

export module engine.app:startup.settings;

import engine.core;

import std;

export namespace pP {
    /// Startup-time validation switches. These must reach initialization
    /// before the RHI device is created, so they resolve at the executable
    /// boundary and are startup-only (never accepted as runtime overrides).
    struct StartupSettings {
        bool m_core_validation{false};
        bool m_gpu_validation{false};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &StartupSettings::m_core_validation,
                    Metadata{
                        "startup.core_validation", "Enable RHI core validation layers", "Startup", EMutability::startup,
                        ESource::all, false},
                    boolean_domain),
                field(
                    &StartupSettings::m_gpu_validation,
                    Metadata{
                        "startup.gpu_validation", "Enable GPU-assisted validation (default off everywhere)", "Startup",
                        EMutability::startup, ESource::all, false},
                    boolean_domain),
            };
        }
    };

    /// CLI bindings owned by this schema for composition into an executable's
    /// combined invocation schema.
    [[nodiscard]] Array<config::OptionDecl> startupCliOptions();
} // namespace pP
