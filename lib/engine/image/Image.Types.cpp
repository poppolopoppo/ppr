module;
#include "pP/Macros.h"

module engine.image;

import :types;
import std;

namespace pP::image {
    class ImageErrorCategory final : public std::error_category {
    public:
        [[nodiscard]] const char *name() const noexcept override {
            return "image";
        }

        [[nodiscard]] std::string message(const int ev) const override {
            switch (static_cast<errc>(ev)) {
                case errc::ok: return "indicates success";
                case errc::invalid_argument: return "indicates that an argument passed in as parameter to a method is invalid";
                case errc::function_not_supported: return "indicates that the requested image operation is not supported for this source";
                default: return "unknown image result (" + std::to_string(ev) + ")";
            }
        }

        [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
            switch (static_cast<errc>(ev)) {
                case errc::invalid_argument: return std::errc::invalid_argument;
                case errc::function_not_supported: return std::errc::function_not_supported;
                default: return {ev, *this};
            }
        }
    };

    [[nodiscard]] const std::error_category &error_category() noexcept {
        static constexpr ImageErrorCategory g_image_error_category{};
        return g_image_error_category;
    }

    [[nodiscard]] std::error_code make_error_code(const errc err) noexcept {
        return std::error_code{static_cast<int>(err), error_category()};
    }
}
