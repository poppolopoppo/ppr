module engine.app;

import :display.settings;

import std;

namespace pP {
    std::error_code DisplaySettings::validate(const DisplaySettings &settings, config::Diagnostics &diagnostics) {
        const auto reject = [&](const std::string_view property, const std::string_view message) {
            const std::error_code error = std::make_error_code(std::errc::invalid_argument);
            diagnostics.add("display settings", std::string{property}, std::string{message}, error);
            return error;
        };
        if (settings.m_cap_hz < 1u or settings.m_cap_hz > 1000u or pacing_domain.nameOf(settings.m_pacing).empty()) {
            return reject("display.pacing", "Invalid pacing mode or cap");
        }
        if (window_mode_domain.nameOf(settings.m_window_mode).empty()) {
            return reject("display.window_mode", "Only windowed and exclusive fullscreen are supported");
        }
        if ((settings.m_width == 0u) != (settings.m_height == 0u) or
            settings.m_width > 2147483647u or
            settings.m_height > 2147483647u or
            settings.m_refresh_hz > 2147483647u) {
            return reject("display.width", "Dimensions must be paired and fit native integer geometry");
        }
        if (settings.m_window_mode == EDisplayWindowMode::windowed and settings.m_refresh_hz != 0u) {
            return reject("display.refresh_hz", "Refresh is an exclusive fullscreen request only");
        }
        if (settings.m_monitor.empty()) {
            return reject("display.monitor", "Use auto, an enumerated monitor identity, or a unique legacy name");
        }
        return {};
    }

    [[nodiscard]] Array<config::OptionDecl> displayCliOptions() {
        Array<config::OptionDecl> options{};

        config::visit(DisplaySettings{}, [&](const auto &member, const auto &value) {
            config::OptionDecl option{};
            option.m_canonical = member.m_metadata.m_name;
            option.m_description = member.m_metadata.m_description;
            option.m_is_switch = std::is_same_v<std::remove_cvref_t<decltype(value)>, bool>;
            if (option.m_canonical == "display.throttle_unfocused") {
                option.m_aliases.push_back("focus-throttle");
                option.m_negations.push_back("no-focus-throttle");
            }
            options.push_back(std::move(option));
        });

        return options;
    }
} // namespace pP
