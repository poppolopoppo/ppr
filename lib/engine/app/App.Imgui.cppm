module;

#include "pP/Macros.h"

export module engine.app:imgui;

import imgui;

export import imgui;

export namespace pP::ui {
    // Engine-side, type-safe handle to an RHI texture view used as an ImGui texture ID.
    // Implicitly converts to `void*`, matching ImGui's `ImTextureID` (defined as `void*` by the
    // static library), so it can be passed directly to ImGui::Image/ImageButton.
    struct TextureId {
        void *m_rhi_texture_view{nullptr};

        constexpr TextureId() noexcept = default;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr TextureId(void *rhi_texture_view) noexcept
            : m_rhi_texture_view(rhi_texture_view) {
        }

        constexpr operator void *() const noexcept { return m_rhi_texture_view; }

        constexpr bool operator==(const TextureId &) const noexcept = default;
    };
}
