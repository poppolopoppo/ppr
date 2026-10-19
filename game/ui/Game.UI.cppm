module;

export module game.ui;

import engine.core;
import engine.math;
import game.colony.world_progress;
import std;

export namespace pP::game::ui {
    // ------------------------------------------------------------------
    // utility window placement
    // ------------------------------------------------------------------

    struct WindowPlacement final {
        float2 m_position{};
        float2 m_size{};
    };

    /// Logical client coordinates. Even a small viewport keeps every edge reachable.
    [[nodiscard]] WindowPlacement fitUtilityWindow(float2 viewport, float2 preferred, float margin) noexcept;

    void prepareUtilityWindow(float preferred_width, float preferred_height, bool constrain_maximum = true);

    // ------------------------------------------------------------------
    // world work and authoring transitions
    // ------------------------------------------------------------------

    enum class EWorldAction : u8 {
        none,
        retry,
        quit,
        cancel
    };

    struct WorldProgressView final {
        colony::WorldJobView m_job{};
        std::string_view m_diagnostic{};
        double m_animation_seconds{};
        u32 m_staged_chunks{};
        u32 m_total_chunks{};
        bool m_open{};
        bool m_closing{};
        bool m_terminal{};
        bool m_published{};
        bool m_activation_pending{};
        bool m_candidate_ready{};
    };

    [[nodiscard]] EWorldAction drawWorldProgress(const WorldProgressView &view);

    [[nodiscard]] bool worldActionAvailable(const WorldProgressView &view, EWorldAction action) noexcept;

    enum class EUnsavedAction : u8 {
        none,
        save,
        discard,
        cancel
    };

    struct UnsavedEditsView final {
        std::string_view m_status{};
        bool m_open{};
        bool m_busy{};
        bool m_saving{};
    };

    [[nodiscard]] EUnsavedAction drawUnsavedEdits(const UnsavedEditsView &view);

    [[nodiscard]] bool unsavedActionAvailable(const UnsavedEditsView &view, EUnsavedAction action) noexcept;

    // ------------------------------------------------------------------
    // frontend observations and projected feedback
    // ------------------------------------------------------------------

    struct FrontendInput final {
        float2 m_pointer{};
        bool m_mouse_captured{};
        bool m_keyboard_captured{};
        bool m_primary_pressed{};
    };

    [[nodiscard]] FrontendInput frontendInput() noexcept;

    enum class EFeedbackRole : u8 {
        selection,
        hover,
        preview
    };
    enum class EFeedbackKind : u8 {
        outline,
        fill,
        line
    };

    /// Already projected by the game owner. No world/camera access occurs while drawing.
    struct FeedbackRect final {
        float2 m_min{};
        float2 m_max{};
        float m_thickness{};
        EFeedbackRole m_role{};
        EFeedbackKind m_kind{};
    };

    struct ChromeBounds final {
        float m_top{};
        float m_bottom{};
        float m_right{};
        float m_left{};
    };

    [[nodiscard]] bool isWorldPoint(float2 point, float2 client_extent, ChromeBounds chrome) noexcept;

    void drawFeedback(std::span<const FeedbackRect> rectangles, float2 client_extent, ChromeBounds chrome);
}
