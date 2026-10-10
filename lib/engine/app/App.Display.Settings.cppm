module;

#include "pP/Macros.h"

export module engine.app:display.settings;

import engine.core;

import std;

export namespace pP {
    enum class EFramePacing : u8 {
        uncapped,
        follow_vsync,
        paced,
    };

    inline constexpr config::EnumDomain<EFramePacing, 3u> pacing_domain{{
        config::EnumEntry<EFramePacing>{EFramePacing::uncapped, "uncapped"},
        config::EnumEntry<EFramePacing>{EFramePacing::follow_vsync, "follow-vsync"},
        config::EnumEntry<EFramePacing>{EFramePacing::paced, "paced"},
    }};

    enum class EDisplayWindowMode : u8 {
        windowed,
        fullscreen
    };

    inline constexpr config::EnumDomain<EDisplayWindowMode, 2u> window_mode_domain{{
        config::EnumEntry<EDisplayWindowMode>{EDisplayWindowMode::windowed, "windowed"},
        config::EnumEntry<EDisplayWindowMode>{EDisplayWindowMode::fullscreen, "fullscreen"},
    }};

    /// Zero dimensions retain window size / select the monitor's current mode.
    /// Zero refresh requests the platform default, not an inferred refresh.
    /// Monitor preference is "auto" or an unambiguous enumerated name, never a handle.
    struct DisplaySettings {
        EFramePacing m_pacing{EFramePacing::follow_vsync};
        u32 m_cap_hz{60u};
        bool m_paced_vsync{true};
        bool m_throttle_unfocused{true};
        EDisplayWindowMode m_window_mode{EDisplayWindowMode::windowed};
        std::string m_monitor{"auto"};
        u32 m_width{0u};
        u32 m_height{0u};
        u32 m_refresh_hz{0u};

        /// Capability-independent cross-field rules, shared by source resolution
        /// and hand-built controller candidates. Capability checks stay app-owned.
        [[nodiscard]] static std::error_code validate(
            const DisplaySettings &settings, config::Diagnostics &diagnostics);

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &DisplaySettings::m_pacing,
                    Metadata{
                        "display.pacing", "Foreground frame pacing mode", "Display", EMutability::runtime, ESource::all,
                        true},
                    pacing_domain),
                field(
                    &DisplaySettings::m_cap_hz,
                    Metadata{
                        "display.cap_hz", "CPU frame cap used in paced mode", "Display", EMutability::runtime,
                        ESource::all, true},
                    Range<u32>{1u, 1000u}),
                field(
                    &DisplaySettings::m_paced_vsync,
                    Metadata{
                        "display.paced_vsync", "Enable presentation synchronization in paced mode", "Display",
                        EMutability::runtime, ESource::all, true},
                    boolean_domain),
                field(
                    &DisplaySettings::m_window_mode,
                    Metadata{
                        "display.window_mode", "Windowed or exclusive fullscreen", "Display", EMutability::runtime,
                        ESource::all, true},
                    window_mode_domain),
                field(
                    &DisplaySettings::m_monitor,
                    Metadata{
                        "display.monitor", "Monitor identity, unique legacy name, or auto (revalidated on enumeration)",
                        "Display", EMutability::runtime, ESource::all, true},
                    any_domain),
                field(
                    &DisplaySettings::m_width,
                    Metadata{
                        "display.width", "Window or exclusive width; zero selects existing/default size", "Display",
                        EMutability::runtime, ESource::all, true},
                    Range<u32>{0u, 2147483647u}),
                field(
                    &DisplaySettings::m_height,
                    Metadata{
                        "display.height", "Window or exclusive height; zero selects existing/default size", "Display",
                        EMutability::runtime, ESource::all, true},
                    Range<u32>{0u, 2147483647u}),
                field(
                    &DisplaySettings::m_refresh_hz,
                    Metadata{
                        "display.refresh_hz", "Exclusive refresh request; zero selects platform default", "Display",
                        EMutability::runtime, ESource::all, true},
                    Range<u32>{0u, 2147483647u}),
                field(
                    &DisplaySettings::m_throttle_unfocused,
                    Metadata{
                        "display.throttle_unfocused", "Limit frame production while unfocused", "Display",
                        EMutability::runtime, ESource::all, true},
                    boolean_domain),
            };
        }
    };

    /// CLI bindings owned by this schema for composition into an executable's
    /// combined invocation schema. `--no-focus-throttle` is the legacy game
    /// spelling and resolves to false on display.throttle_unfocused.
    [[nodiscard]] Array<config::OptionDecl> displayCliOptions();
} // namespace pP
