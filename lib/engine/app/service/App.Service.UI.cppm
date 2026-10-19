module;
#include "pP/Macros.h"
export module engine.app:service.ui;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    class InputListener;
    class WindowInputContext;
    class WindowViewport;
    struct DetachedUiDrawData;
    struct DrawContext;

    // ------------------------------------------------------------------
    // ui fonts
    // ------------------------------------------------------------------

    /// One inclusive codepoint span contributed by a merged font source.
    struct UIFontGlyphRange final {
        char32_t m_first{0u};
        char32_t m_last{0u};
    };

    /// One static font input: the path is read through the engine I/O layer
    /// and the bytes are validated against the pinned exact size and SHA-256
    /// digest before any ImGui atlas call. The service owns the bytes for
    /// the whole atlas lifetime (ImFontConfig::FontDataOwnedByAtlas = false).
    ///
    /// A merge source (`m_merge_into_previous`) adds its declared glyphs to
    /// the preceding face instead of creating a font of its own, so icon
    /// sets come after the face they extend and never reorder the face list.
    /// It must declare at least one span in `m_merge_glyphs`; the engine
    /// copies the spans into range storage it owns for the atlas lifetime
    /// while loading, so the referenced array only has to stay valid for the
    /// duration of the `initialize` call — it does not need to outlive it (a
    /// static or path-owning array literal remains the natural form). The
    /// merge reference size is `m_size_pixels`: ImGui scales merged glyphs
    /// by its ratio against the destination face, so passing the destination
    /// size bakes merged glyphs 1:1 with the face they extend.
    struct UIFontSource final {
        std::filesystem::path m_path{};
        std::span<const UIFontGlyphRange> m_merge_glyphs{};
        u64 m_expected_bytes{0u};
        float m_size_pixels{0.0f};
        std::array<std::byte, 32> m_sha256{};
        bool m_merge_into_previous{false};
    };

    /// Decodes a 64-digit hex SHA-256 digest. Rejects malformed digits and
    /// any length other than 64 (checked at compile time for literals).
    [[nodiscard]] constexpr std::array<std::byte, 32> sha256DigestFromHex(const std::string_view hex) noexcept {
        PPR_ASSERT(hex.size() == 64u);

        const auto nibble = [](const char c) constexpr noexcept -> unsigned {
            if (c >= '0' and c <= '9') {
                return static_cast<unsigned>(c - '0');
            }
            if (c >= 'a' and c <= 'f') {
                return static_cast<unsigned>(c - 'a') + 10u;
            }
            if (c >= 'A' and c <= 'F') {
                return static_cast<unsigned>(c - 'A') + 10u;
            }
            PPR_ASSERT(false);
            return 0u;
        };

        std::array<std::byte, 32> digest{};
        for (std::size_t byte_index = 0u; byte_index < digest.size(); ++byte_index) {
            const unsigned high = nibble(hex[byte_index * 2u]);
            const unsigned low = nibble(hex[byte_index * 2u + 1u]);
            digest[byte_index] = static_cast<std::byte>((high << 4u) | low);
        }
        return digest;
    }

    // ------------------------------------------------------------------
    // ui service
    // ------------------------------------------------------------------

    class IUIService : public virtual IService {
    public:
        /// Initializes the service. `fonts` names the static font set to bake
        /// (see UIFontSource); an empty span keeps the stock ImGui default.
        [[nodiscard]] virtual std::error_code initialize(
            WindowInputContext &window_input_context, IRhiService &rhi_service, IShaderService &shader_service,
            std::span<const UIFontSource> fonts, int input_listener_priority) = 0;

        [[nodiscard]] virtual std::error_code shutdown() = 0;

        // Cached mouse-capture flag for press-time sampling.
        [[nodiscard]] virtual bool hasMouseCaptureUnlessPopupClose() const noexcept = 0;

        // Clears UI window focus; call post-update with open frame.
        [[nodiscard]] virtual std::error_code clearWindowFocus() = 0;

        [[nodiscard]] virtual std::error_code update(TimeSpan dt, const WindowViewport &viewport) = 0;

        [[nodiscard]] virtual std::error_code render(const DrawContext &draw_context) = 0;

        // P2 frontend/backend split. buildDetachedUiFrame runs on the CLIENT
        // lane: it completes ImGui::Render() and publishes detached owned
        // draw data (deep-copied verts/indices/commands, baked clip/scale,
        // staged font pixels on rebuild). renderDetachedUiFrame runs on ANY
        // lane (inline or worker): it encodes ONLY the detached data plus
        // lane-confined GPU state, never the live context/IO/GetDrawData().
        // Font/texture create-replace carries explicit ownership: the
        // client stages owned pixels + a version, the lane installs them
        // and reports the applied version for acknowledgement.
        [[nodiscard]] virtual std::error_code buildDetachedUiFrame(DetachedUiDrawData &out) = 0;

        [[nodiscard]] virtual std::error_code renderDetachedUiFrame(
            const DetachedUiDrawData &detached, const DrawContext &draw_context) = 0;

        // Last font atlas version installed on the encode lane (the ack
        // the frame result carries back to the client).
        [[nodiscard]] virtual u64 installedFontVersion() const noexcept = 0;

        [[nodiscard]] virtual void *getContext() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<const InputListener> getInputListener() const noexcept = 0;
    };
}
