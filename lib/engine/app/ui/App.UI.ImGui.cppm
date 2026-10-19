module;

#include "pP/Macros.h"

export module engine.app:ui.imgui;

import :renderer.frame_fence;
import :service.ui;

import engine.core;
import std;
import engine.math;
import imgui;

export namespace pP {
    class IUIService;

    namespace ui {
        [[nodiscard]] std::unique_ptr<IUIService> createImGuiService();

        // ------------------------------------------------------------------
        // Site Office tokens
        // ------------------------------------------------------------------

        /// One authored Site Office color from the plan's token table: 8-bit
        /// sRGB channels at 100% scale. ImGui and detached vertices retain the
        /// authored bytes. The UI vertex shader decodes RGB before interpolation
        /// only for an sRGB draw target; alpha remains linear coverage.
        struct ThemeColor final {
            u8 m_r{0u};
            u8 m_g{0u};
            u8 m_b{0u};

            [[nodiscard]] constexpr ImVec4 vec4(const float alpha = 1.0f) const noexcept {
                return ImVec4{
                    static_cast<float>(m_r) / 255.0f, static_cast<float>(m_g) / 255.0f,
                    static_cast<float>(m_b) / 255.0f, alpha};
            }
        };

        /// The Site Office palette, adapted from the rounded concept board. The
        /// private style mapping in the ImGui service consumes the full set;
        /// presentation that owns a semantic status color (display errors,
        /// timing reference lines) reads these tokens instead of restating
        /// hex values, so re-validating the table stays a single-point change.
        namespace theme {
            /// Base-face order supplied by ApplicationEditor; merged icons
            /// belong to the body face only, so the icon merge never consumes a
            /// slot. `tiny` binds the body family a second time at a smaller,
            /// narrower size for compact read-outs; it carries no icon merge,
            /// so icon-bearing text stays on `body`.
            enum class EFont : u8 {
                body,
                emphasis,
                heading,
                tiny
            };

            /// Reacquire each draw: DPI rebuilds replace atlas objects. Stock-font
            /// contexts intentionally use their current font; custom load errors
            /// are handled at service initialization, never by this accessor.
            [[nodiscard]] ImFont *font(EFont role) noexcept;

            inline constexpr ThemeColor kCanvas{0xCFu, 0xC7u, 0xABu};
            inline constexpr ThemeColor kPanel{0xD9u, 0xD2u, 0xB4u};
            inline constexpr ThemeColor kRaised{0xF4u, 0xEEu, 0xD6u};
            inline constexpr ThemeColor kPaper{0xFBu, 0xF7u, 0xE6u};
            inline constexpr ThemeColor kHover{0xE9u, 0xDEu, 0xB8u};
            inline constexpr ThemeColor kBorder{0x8Au, 0x7Fu, 0x5Eu};
            inline constexpr ThemeColor kBorderStrong{0x25u, 0x22u, 0x1Cu};
            inline constexpr ThemeColor kTextPrimary{0x25u, 0x22u, 0x1Cu};
            inline constexpr ThemeColor kTextSecondary{0x5Au, 0x54u, 0x44u};
            inline constexpr ThemeColor kAccent{0xFFu, 0xC7u, 0x2Cu};
            inline constexpr ThemeColor kAccentHover{0xFFu, 0xD7u, 0x68u};
            inline constexpr ThemeColor kAccentActive{0xE8u, 0xB0u, 0x12u};
            inline constexpr ThemeColor kAccentText{0x25u, 0x22u, 0x1Cu};
            inline constexpr ThemeColor kSelection{0x3Fu, 0x7Fu, 0xD6u};
            inline constexpr ThemeColor kSelectionText{0x20u, 0x4Bu, 0x83u};
            inline constexpr ThemeColor kSuccess{0x2Fu, 0x68u, 0x2Bu};
            inline constexpr ThemeColor kWarning{0x78u, 0x4Cu, 0x08u};
            inline constexpr ThemeColor kError{0xA3u, 0x30u, 0x16u};

            /// Background alpha for movable informational windows: low enough
            /// that the scene reads through the panel, high enough that body
            /// text keeps its authored contrast. Fixed chrome bands, popups,
            /// menus and modals stay at full alpha.
            inline constexpr float kFloatingPanelAlpha{0.86f};
        }

        // ------------------------------------------------------------------
        // icon vocabulary
        // ------------------------------------------------------------------

        /// Font Awesome 7 Free Solid codepoints merged into the body face by
        /// the Editor font list — the complete set any surface references,
        /// and therefore the exact set baked. Command labels include words;
        /// compact rail buttons expose their actual tool/shortcut in tooltips.
        /// Asset tiles retain readable names and kind metadata beside the symbol.
        namespace icons {
            inline constexpr char32_t kClose{0xF00Du}; // xmark
            inline constexpr char32_t kReload{0xF021u}; // arrows-rotate
            inline constexpr char32_t kSave{0xF0C7u}; // floppy-disk
            inline constexpr char32_t kUndo{0xF2EAu}; // rotate-left
            inline constexpr char32_t kRedo{0xF2F9u}; // rotate-right
            inline constexpr char32_t kCubes{0xF1B3u};
            inline constexpr char32_t kLeaf{0xF06Cu};
            inline constexpr char32_t kSliders{0xF1DEu};
            inline constexpr char32_t kMountain{0xF6FCu};
            inline constexpr char32_t kGlobe{0xF0ACu};
            inline constexpr char32_t kImage{0xF03Eu};
            inline constexpr char32_t kLayers{0xF5FDu};
            inline constexpr char32_t kFile{0xF15Bu};
            inline constexpr char32_t kPlay{0xF04Bu};
            inline constexpr char32_t kPause{0xF04Cu};
            inline constexpr char32_t kStep{0xF051u};
            inline constexpr char32_t kInspect{0xF05Bu};
            inline constexpr char32_t kDig{0xF6E3u};
            inline constexpr char32_t kDemolish{0xF05Eu};
            inline constexpr char32_t kSettings{0xF013u};
            inline constexpr char32_t kChart{0xF201u};
            inline constexpr char32_t kFolder{0xF07Cu};
            inline constexpr char32_t kHelp{0xF059u};


            /// One inclusive span per referenced codepoint, handed to the
            /// merge source (`UIFontSource::m_merge_glyphs`) so exactly the
            /// vocabulary above is baked. The vocabulary test proves the
            /// union of these spans equals the codepoint set.
            inline constexpr std::array kMergeRanges{
                UIFontGlyphRange{.m_first = kClose, .m_last = kClose},
                UIFontGlyphRange{.m_first = kReload, .m_last = kReload},
                UIFontGlyphRange{.m_first = kSave, .m_last = kSave},
                UIFontGlyphRange{.m_first = kUndo, .m_last = kUndo},
                UIFontGlyphRange{.m_first = kRedo, .m_last = kRedo},
                UIFontGlyphRange{.m_first = kCubes, .m_last = kCubes},
                UIFontGlyphRange{.m_first = kLeaf, .m_last = kLeaf},
                UIFontGlyphRange{.m_first = kSliders, .m_last = kSliders},
                UIFontGlyphRange{.m_first = kMountain, .m_last = kMountain},
                UIFontGlyphRange{.m_first = kGlobe, .m_last = kGlobe},
                UIFontGlyphRange{.m_first = kImage, .m_last = kImage},
                UIFontGlyphRange{.m_first = kLayers, .m_last = kLayers},
                UIFontGlyphRange{.m_first = kFile, .m_last = kFile},
                UIFontGlyphRange{.m_first = kPlay, .m_last = kPlay},
                UIFontGlyphRange{.m_first = kPause, .m_last = kPause},
                UIFontGlyphRange{.m_first = kStep, .m_last = kStep},
                UIFontGlyphRange{.m_first = kInspect, .m_last = kInspect},
                UIFontGlyphRange{.m_first = kDig, .m_last = kDig},
                UIFontGlyphRange{.m_first = kDemolish, .m_last = kDemolish},
                UIFontGlyphRange{.m_first = kSettings, .m_last = kSettings},
                UIFontGlyphRange{.m_first = kChart, .m_last = kChart},
                UIFontGlyphRange{.m_first = kFolder, .m_last = kFolder},
                UIFontGlyphRange{.m_first = kHelp, .m_last = kHelp},

            };

            /// Exact button labels: "<icon> <words>###<pre-icon id>". The
            /// ### sequence makes ImGui hash only the id part (imgui.cpp
            /// ImHashStr drops the prefix), so each label keeps the widget
            /// ID it had before the icon while showing the icon up front.
            inline constexpr std::string_view kCloseLabel{"\uf00d Close###Close"};
            inline constexpr std::string_view kReloadLabel{"\uf021 Reload###Reload"};
            inline constexpr std::string_view kSaveDocumentsLabel{"\uf0c7 Save documents###Save documents"};
            inline constexpr std::string_view kUndoLabel{"\uf2ea Undo###Undo"};
            inline constexpr std::string_view kRedoLabel{"\uf2f9 Redo###Redo"};
            inline constexpr std::string_view kReloadDescriptorLabel{
                "\uf021 Reload descriptor text###Reload descriptor text"};

            inline constexpr std::string_view kCubesLabel{"\uf1b3 Materials###asset_material"};
            inline constexpr std::string_view kLeafLabel{"\uf06c Biomes###asset_biome"};
            inline constexpr std::string_view kSlidersLabel{"\uf1de Fields and rules###asset_field"};
            inline constexpr std::string_view kMountainLabel{"\uf6fc Features###asset_feature"};
            inline constexpr std::string_view kGlobeLabel{"\uf0ac Worlds###asset_world"};
            inline constexpr std::string_view kImageLabel{"\uf03e Textures###asset_texture"};
            inline constexpr std::string_view kLayersLabel{"\uf5fd Backgrounds###asset_background"};
            inline constexpr std::string_view kFileLabel{"\uf15b Asset###asset_unknown"};
            inline constexpr std::string_view kPlayLabel{"\uf04b Resume###Resume"};
            inline constexpr std::string_view kPauseLabel{"\uf04c Pause###Pause"};
            inline constexpr std::string_view kStepLabel{"\uf051 Step###Step"};
            inline constexpr std::string_view kInspectLabel{"\uf05b Inspect###tool0"};
            inline constexpr std::string_view kDigLabel{"\uf6e3 Dig###tool1"};
            inline constexpr std::string_view kDemolishLabel{"\uf05e Demolish###tool3"};
            inline constexpr std::string_view kSettingsLabel{"\uf013 Display###Display"};
            inline constexpr std::string_view kChartLabel{"\uf201 Performance###Performance"};
            inline constexpr std::string_view kFolderLabel{"\uf07c Workspace###Workspace"};
            inline constexpr std::string_view kHelpLabel{"\uf059 Help###Help"};

            /// Every label above, for the vocabulary test: each label's
            /// leading codepoint must be baked and its hash must equal the
            /// hash of the id part after "###".
            inline constexpr std::array kLabels{
                kCloseLabel, kReloadLabel, kSaveDocumentsLabel, kUndoLabel, kRedoLabel, kReloadDescriptorLabel,
                kCubesLabel, kLeafLabel, kSlidersLabel, kMountainLabel, kGlobeLabel, kImageLabel, kLayersLabel,
                kFileLabel, kPlayLabel, kPauseLabel, kStepLabel, kInspectLabel, kDigLabel, kDemolishLabel,
                kSettingsLabel, kChartLabel, kFolderLabel, kHelpLabel};

            /// Icon-only help affordance: the top bar's Help glyph without its
            /// words, so a caveat beside a label costs one baked glyph and the
            /// explanation rides the hover tooltip. A text marker rather than a
            /// button label - no "###" id part, and deliberately outside
            /// kLabels, whose entries must carry readable words.
            inline constexpr std::string_view kHelpMarkerGlyph{"\uf059"};
        }

        // ------------------------------------------------------------------
        // panel chrome
        // ------------------------------------------------------------------

        /// The one header a decorated panel is allowed: the heading face for the
        /// title bar alone. ImGui draws that title bar inside `Begin` with
        /// whatever face is current there (imgui.cpp:8731 calls
        /// RenderWindowTitleBarContents) and never restores the previous one, so
        /// the face must be popped again before the body draws.
        ///
        /// Construct into a NON-const object immediately before `Begin`, call
        /// `close()` immediately after it returns, on every branch:
        ///
        ///     WindowTitleFont title_font{};
        ///     const bool visible = ImGui::Begin("Panel", &open, flags);
        ///     title_font.close();
        ///     if (not visible) { return std::nullopt; }  // already balanced
        ///     // body content draws on the face the caller had
        ///
        /// The same shape covers `BeginPopupModal`. Letting the destructor run
        /// only at the end of the begin block instead would render the whole
        /// body in the heading face; `close()` exists so an early return cannot
        /// forget the pop. Windows drawn with `ImGuiWindowFlags_NoTitleBar`
        /// render no header and must not construct this. In stock-font contexts
        /// the heading role resolves to the current face, so the push/pop pair
        /// is then a no-op.
        class WindowTitleFont final {
            bool m_pushed{false};

        public:
            WindowTitleFont() noexcept;
            WindowTitleFont(const WindowTitleFont &) = delete;
            WindowTitleFont(WindowTitleFont &&) = delete;
            WindowTitleFont &operator=(const WindowTitleFont &) = delete;
            WindowTitleFont &operator=(WindowTitleFont &&) = delete;
            ~WindowTitleFont() noexcept;

            /// Restore the face in effect before construction. Idempotent; the
            /// destructor calls it.
            void close() noexcept;
        };

        /// Compact help affordance: the baked Font Awesome circle-question with
        /// the explanation in its hover tooltip, so a caveat costs one glyph
        /// instead of an always-visible note line. The glyph is drawn on the
        /// body face whatever the caller has pushed, because the icon merge
        /// lives on that face alone — `emphasis`, `heading` and `tiny` bake no
        /// icon glyphs and would render the marker as a missing-glyph box.
        /// `tooltip` is a plain string: callers with dynamic text format it
        /// first and pass the buffer.
        void helpMarker(const char *tooltip);
    }

    // P2 detached UI draw data: everything the render lane needs to encode
    // one frame's UI, owned and imgui-free. Published by the client AFTER
    // ImGui::Render() completes; immutable afterwards. The worker never
    // reads the live context, live IO, GetDrawData(), or mutable frontend
    // backend fields — only this struct.
    struct DetachedUiVertex {
        float2 m_position{};
        float2 m_uv{};
        u32 m_color{0u};
    };

    struct DetachedUiCommand {
        // Pre-scaled clip rectangle in framebuffer pixels (DisplayPos and
        // framebuffer scale are baked at detach time, never re-read live).
        float4 m_clip_rect{};
        // Opaque texture identity as observed at detach (the font atlas
        // view for the stock backend). Unknown identities are rejected at
        // encode, never dereferenced.
        u64 m_texture_id{0u};
        u32 m_element_count{0u};
        u32 m_index_offset{0u};
        u32 m_vertex_offset{0u};
        // Known render-only reset command (reapplies render state at this
        // position); any other client-context callback was already rejected
        // at detach and never reaches the lane.
        bool m_reset_render_state{false};
    };

    struct DetachedUiDrawData {
        u64 m_frame_id{0u};
        float2 m_display_size{};
        float2 m_framebuffer_scale{1.0f, 1.0f};
        float2 m_framebuffer_extent{};
        Array<DetachedUiVertex> m_vertices{};
        Array<u32> m_indices{};
        Array<DetachedUiCommand> m_commands{};
        // Font atlas publication: nonzero only when the client rebuilt the
        // atlas (DPI/scale change). The lane uploads the pixels, installs
        // them, and acknowledges m_font_version in the frame result. Empty
        // pixels mean "keep the installed atlas".
        u64 m_font_version{0u};
        int2 m_font_extent{zero_v};
        Array<u8> m_font_pixels{};
        bool m_has_content{false};
    };

    // Fence-awareness seam for UI backends behind IUIService: the frame
    // owner reports the SUBMIT's fence value after a successful submit so
    // the backend can key its upload-slot reuse off submission completion.
    // Queried via dynamic_cast (same shape as the GlfwInput seam in the
    // Editor); a backend that does not implement it simply keeps its own
    // rule. Render-thread confined like every other UI call.
    class IFenceAwareUiService {
    public:
        virtual ~IFenceAwareUiService() = default;

        virtual void notifyFrameSubmitted(FenceCompletionValue submit_value) noexcept = 0;
    };

    [[nodiscard]] ImVec2 imVec(const float2 &v) noexcept {
        return ImVec2{v.x, v.y};
    }

    [[nodiscard]] ImVec4 imVec(const float4 &v) noexcept {
        return ImVec4{v.x, v.y, v.z, v.w};
    }

    [[nodiscard]] float2 imVec(const ImVec2 &v) noexcept {
        return float2{v.x, v.y};
    }

    [[nodiscard]] float4 imVec(const ImVec4 &v) noexcept {
        return float4{v.x, v.y, v.z, v.w};
    }
}
