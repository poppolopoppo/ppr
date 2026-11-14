module;
#include "pP/Macros.h"
export module engine.app:renderer.grid_pass;

import :renderer.types;
import :renderer.frame_fence;
import :renderer.gpu_caches;
import :renderer.world_lighting;
import :renderer.world_lighting_pass;
import :scene.camera;

import engine.core;
import engine.image;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    PPR_DECLARE_LOG_CATEGORY(GridPass)

    // ------------------------------------------------------------------
    // grid pass: chunk-sized tile quads over the cell world
    // ------------------------------------------------------------------
    // Content-specific pass owning its shader/pipeline/buffers/GPU tile
    // cache; Renderer stays content-free. Consumes CameraSnapshot (never a
    // mutable Camera while drawing); viewport and scissor travel per-draw on
    // DrawSubmission; row-major row-vector LH +Z-forward +Y-up, [0,1] depth,
    // mul(float4,matrix), view*projection, no backend transpose/Y-flip.
    // Content-owned immutable lighting leases are optional explicit inputs.
    //
    // Tiles reuse the single 32x32-chunk x 128x128-cell geometry as
    // CHUNK-SIZED tiles (kTilesPerChunk == 1): one tile per chunk, never a
    // second tiling system. Finer tiling is unjustified: 1024 frustum tests
    // per frame are trivially cheap next to a second tile hierarchy.
    // Frustum/visibility runs per tile from the snapshot bounds; quiescent
    // (clean) and off-screen tiles are skipped by the plan.
    //
    // Graph note: game/colony (which owns the engine.sim link) translates
    // grid state into these plain structs; engine.app gains NO engine.sim
    // dependency and no sim type crosses this boundary.

    // Tile rect in world-cell units. Signed so camera-centred test layouts
    // stay representable; production submissions always land in [0, edge).
    struct GridTileRange {
        i32 m_min_x = 0;
        i32 m_min_y = 0;
        i32 m_max_x = 0;
        i32 m_max_y = 0;
    };

    // Owned numeric catalog visuals. Texture indices name the immutable world
    // bundle's source images; they are distinct from terrain material IDs.
    inline constexpr u32 kGridSurfaceOpticalBlend = 1u;
    inline constexpr u32 kGridSurfaceTextureRepeat = 2u;
    inline constexpr u32 kGridSurfaceParticleSolid = 4u;
    inline constexpr u32 kGridSurfaceParticleLiquid = 8u;

    // Tagged version1 authored transport, independent of visual alpha/IOR/phase.
    inline constexpr u32 kGridTransportUnavailable = 0u;
    inline constexpr u32 kGridTransportVacuumV1 = 0x101u;
    inline constexpr u32 kGridTransportOpaqueV1 = 0x102u;
    inline constexpr u32 kGridTransportAbsorbingV1 = 0x103u;

    struct alignas(16) GridTerrainVisual {
        u32 m_material_id{};
        u32 m_color_rgba{};
        u32 m_texture_index{0xffffffffu};
        float m_texture_period_m{1.0f};
        float m_roundness{1.0f};
        // Derived from admitted albedo bytes when the immutable GPU palette is built.
        float m_filtered_mean_rgb[3]{1.0f, 1.0f, 1.0f};
        u32 m_normal_texture_index{0xffffffffu};
        float m_metallic{};
        float m_roughness{1.0f};
        float m_opacity{1.0f};
        float m_emissive_rgb[3]{};
        float m_emissive_strength{};
        float m_ior{1.0f};
        float m_normal_scale{1.0f};
        float m_refraction_depth_m{};
        u32 m_surface_flags{};
        u32 m_transport_model{kGridTransportUnavailable};
        float m_sigma_per_m[3]{};
    };
    static_assert(sizeof(GridTerrainVisual) == 96u);
    static_assert(std::is_standard_layout_v<GridTerrainVisual> and std::is_trivially_copyable_v<GridTerrainVisual>);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_transport_model) == 80u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_sigma_per_m) == 84u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_normal_texture_index) == 32u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_metallic) == 36u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_roughness) == 40u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_opacity) == 44u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_emissive_rgb) == 48u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_emissive_strength) == 60u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_ior) == 64u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_normal_scale) == 68u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_refraction_depth_m) == 72u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_surface_flags) == 76u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_roundness) == 16u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_filtered_mean_rgb) == 20u);
    static_assert(alignof(GridTerrainVisual) == 16u);

    // Visual-only unavailable rows remain admissible with zero sigma.
    [[nodiscard]] std::error_code validateGridMaterialOptics(const GridTerrainVisual &visual);

    // ------------------------------------------------------------------
    // packed particle presentation row
    // ------------------------------------------------------------------

    // Material ids ride the presentation row in 12 bits, so the particle domain
    // is capped at 4096 (16x the authored 256) even though the palette and the
    // resolved-cell word address 16 bits. A larger id is rejected rather than
    // truncated into a different valid material.
    inline constexpr u32 kGridParticleMaterialLimit{4096u};

    // Renderer-owned, packed 24-byte presentation row; staging traffic only, not
    // part of the solver ABI or any packet. The shader mirrors are
    // `PresentationRow` in grid_particle_prepare.slang (writer) and `ParticleGpu`
    // in grid_tiles.slang (reader).
    //
    // Position and radius stay binary32 in world metres: the float32 ulp at the
    // 1365 m map corner is 4.1e-3 px at span64, where an absolute half2 position
    // would be 8.44 px. The quantised fields are the material/class/temperature
    // word, the RGB9E5 tint and the binary16 motion pair.
    struct alignas(8) ParticleGpu {
        float m_position[2]{};
        float m_radius_m{};
        // material 12 bits | class flags 4 bits << 12 | binary16 Kelvin << 16.
        // The material is not stored in its canonical packCell position; shading
        // reconstructs `material | (temperature << 16)`.
        u32 m_material_flags_temperature{};
        // RGB9E5 tint from the palette's RGBA8 colour; alpha is not stored.
        u32 m_color_rgb9e5{};
        // binary16 pair: native cells per solver step, x low word / y high word.
        u16 m_motion[2]{};
    };
    static_assert(sizeof(ParticleGpu) == 24u and alignof(ParticleGpu) == 8u);
    static_assert(PPR_OFFSETOF(ParticleGpu, m_position) == 0u);
    static_assert(PPR_OFFSETOF(ParticleGpu, m_radius_m) == 8u);
    static_assert(PPR_OFFSETOF(ParticleGpu, m_material_flags_temperature) == 12u);
    static_assert(PPR_OFFSETOF(ParticleGpu, m_color_rgb9e5) == 16u);
    static_assert(PPR_OFFSETOF(ParticleGpu, m_motion) == 20u);
    static_assert(std::is_standard_layout_v<ParticleGpu> and std::is_trivially_copyable_v<ParticleGpu>);

    // ------------------------------------------------------------------
    // per-particle presentation orientation history
    // ------------------------------------------------------------------

    // Renderer-owned 4-byte per-particle orientation record: presentation history
    // only, not part of the solver ABI, the presentation row or any packet. Two
    // transactional roles (accepted and candidate) cost 8 B per capacity row, or
    // 32 KiB / 512 KiB / 8 MiB at 4096 / 65536 / 1048576 rows. The record holds
    // canonical angle16 (bits 0..15), bias strength B (16..23) and stretch excess S
    // (24..31). The angle is the history; the vertex stage decodes it to (cos, sin)
    // for drawing and never writes that decoded pair back. The 4-byte stride is
    // asserted here and reflected at program creation. The shader twin is
    // `OrientationRow` in grid_particle_native.slang.
    struct alignas(4) ParticleOrientationGpu {
        u32 m_angle_bias_stretch{};
    };
    static_assert(sizeof(ParticleOrientationGpu) == 4u and alignof(ParticleOrientationGpu) == 4u);
    static_assert(PPR_OFFSETOF(ParticleOrientationGpu, m_angle_bias_stretch) == 0u);
    static_assert(
        std::is_standard_layout_v<ParticleOrientationGpu> and std::is_trivially_copyable_v<ParticleOrientationGpu>);

    // ------------------------------------------------------------------
    // particle presentation shapes
    // ------------------------------------------------------------------
    // One constant set, mirrored by grid_particle_native.slang (the prepare cull
    // and the draw geometry read it there). r is the packed physical radius
    // h*sqrt(V/pi); R = 2.0 r is the support radius both phases share (the P1
    // decision); the cull margin in the prepare `visible()` test is 3.0625 r,
    // which also covers the retained 1.5x flow stretch at 3.000 r.
    inline constexpr float kGridParticleSupportScale{2.0f};
    inline constexpr float kGridParticleCullRadiusScale{3.0625f};
    inline constexpr float kGridParticleStretchCap{1.5f};
    // Magnitude reference of the retained flow cue, in native cells per solver
    // step. Fixed and illustrative, not a physical speed: the cue reads a per-cell
    // motion and introduces no dt. 0.06 makes ordinary flow saturate the cap. The
    // cue's input is the bilinearly sampled per-cell winner-motion field
    // (grid_particle_prepare.slang particleFlowMotion), not one row's own
    // displacement, so a neighbour's stretch changes continuously in position
    // (Task 4 continuity fix); the prepare pass resolves it once per accepted
    // publication and this twin carries only the mapping.
    inline constexpr float kGridParticleMotionReference{0.06f};

    // One C1 radial falloff for both phases, from rho = |p - centre| / R: alpha is
    // 1 for rho <= 2/3, smoothstep(1, 2/3, rho) for 2/3 < rho < 1 and 0 beyond.
    // Both restored classes composite this falloff premultiplied as their edge
    // coverage, so the silhouette is the coverage itself rather than a discard at
    // alpha = 0.5; its resting half-coverage contour is rho = 5/6, and the
    // optional tangent cue moves that contour per angle inside [2/3, 1] and
    // touches neither join. Every seeded-lattice point sits inside R_in, so a
    // packed interior's *radial falloff* is saturated; the final solid coverage
    // also multiplies the SDF rim and the terrain complement, so it is exactly 1
    // only where those leave the plateau untouched.
    inline constexpr float kGridParticleFalloffInnerFraction{2.0f / 3.0f};
    // Negligible-fringe floor for depth writes, not a shape threshold: a fragment
    // is cut only when its premultiplied edge coverage is at or below this, so the
    // C1 falloff's vanishing tail writes nothing while the silhouette keeps its
    // analytic coverage. The tail ends ≈0.077 px inside the support at span64
    // (≈0.022 px at span220), below any 8-bit LSB.
    inline constexpr float kGridParticleCoverageFloor{1.0f / 255.0f};

    // Tangent-aligned contour cue (Task 11 increment 1, appearance only). The
    // half-coverage contour radius, in rho units, is
    // k(theta) = k0 * (1 + b_eff*(cos^2(theta) - 1/2)) measured from the surface
    // tangent read from the committed per-cell Q16 fill field. b_eff = b *
    // strength, where strength is the energy gate above, so an ambiguous field
    // renders round (b_eff = 0, k(theta) = k0 exactly) and b_eff never exceeds b.
    // k0 = 5/6 is the resting 0.5 contour. b = 0.3999 is the strongest
    // four-decimal bias whose endpoints stay strictly inside the legal band in
    // binary32 - k_max = 0.999958277 along the tangent and k_min = 0.666708350
    // across it, a 1.49984 : 1 aspect (1.49971 : 1 after the defensive clamp).
    // b = 0.40 is degenerate rather than merely stronger: 0.83333331f *
    // 0.80000001f rounds to exactly float32(2/3), so k_min would sit ON the
    // plateau while the shader's warp divides by (contour - 2/3). k_min > 2/3 is
    // the *finite-band* requirement; k_max <= 1 is the *support* requirement.
    // Both ends inside [2/3, 1] means only the band is reshaped: plateau, support
    // radius R, interior coverage, the diagonal seam and the 3.0625 r cull are
    // untouched for any orientation. Fixed source values; no tuning UI and no
    // second strength knob.
    inline constexpr float kGridParticleTangentContourScale{5.0f / 6.0f};
    inline constexpr float kGridParticleTangentContourBias{0.3999f};
    // Defensive clamp of the contour radius, in rho units: one 1e-4 hair above
    // the plateau and the support exactly. With the maximum b it is nearly inert -
    // it bites only within about 0.76 degrees of the normal direction, raising
    // k_min from 0.666708 to 0.666767 - and any weaker b_eff is more inert still.
    // It removes any possibility of a zero band offset or of a contour at the
    // support for any legal bias.
    inline constexpr float kGridParticleTangentContourMinimum{kGridParticleFalloffInnerFraction + 1.0e-4f};
    // A gradient magnitude at or below this is "no surface": the numerical guard
    // against dividing by zero, not a confidence threshold. One particle-sized
    // fill quantum (0.25 cell of volume) changes one stencil cell and moves the
    // gradient by at most 0.5 in these units, far above it; a flat field is
    // exactly 0.
    inline constexpr float kGridParticleTangentGradientEpsilon{1.0e-4f};
    // Energy gate on that gradient, in the unnormalised Sobel units of
    // grid_tiles.slang particleSurfaceTangent: a full-fill step is |grad| = 4, a
    // quarter-fill step 1 and a shallow 1/32 ramp 0.25. The cue strength is
    // smoothstep(1, 3, |grad|), scaling b toward 0. These are *proposed* values
    // for P2 appearance calibration, not measured thresholds.
    inline constexpr float kGridParticleTangentConfidenceLow{1.0f};
    inline constexpr float kGridParticleTangentConfidenceHigh{3.0f};
    // Round-off closure of the tangent contour's two joins, relative to R. |local|
    // is a rounded length, so a sample constructed exactly at the support radius can
    // arrive a few ulps inside it and the warp would return a denormal alpha there
    // instead of 0. This is a numerical closure, not a shape or strength knob: about
    // eight binary32 ulps at rho = 1 (1.2e-7) and 1e-6 R, i.e. 0.002 px at span64.
    inline constexpr float kGridParticleTangentJoinClosure{1.0e-6f};

    // Solid: a rounded square inscribed in the R circle. It is the Minkowski sum
    // of a square of half-extent a-c and a disk of radius c, so its support is
    // S = sqrt(2)*a - c*(sqrt(2)-1). With c = 0.25 a, S = 1.3106 a, giving
    // a = R/1.3106 = 1.5260 r (0.4305 cell at r = 0.2821 cell) and
    // c = 0.25 a (0.1076 cell). Liquid: a disk of radius R, support exactly R.
    inline constexpr float kGridParticleRoundedSquareSupport{1.3106f};
    inline constexpr float kGridParticleSolidHalfExtentScale{
        kGridParticleSupportScale / kGridParticleRoundedSquareSupport};
    inline constexpr float kGridParticleSolidCornerFraction{0.25f};
    inline constexpr float kGridParticleSolidCornerScale{
        kGridParticleSolidCornerFraction * kGridParticleSolidHalfExtentScale};
    inline constexpr float kGridParticleSolidInnerHalfExtentScale{
        kGridParticleSolidHalfExtentScale - kGridParticleSolidCornerScale};

    // Support radius of a rounded square of half-extent `half_extent` and corner
    // radius `corner`, in units of r.
    [[nodiscard]] constexpr float gridParticleRoundedSquareSupport(
        const float half_extent, const float corner) noexcept {
        constexpr float root_two = 1.41421356237309505f;
        return root_two * half_extent - corner * (root_two - 1.0f);
    }

    // C1 falloff mirror of grid_tiles.slang's particleFalloff, in rho units.
    [[nodiscard]] constexpr float gridParticleFalloff(const float rho) noexcept {
        const float t = (1.0f - rho) / (1.0f - kGridParticleFalloffInnerFraction);
        const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return clamped * clamped * (3.0f - 2.0f * clamped);
    }

    // Every shape's support must stay inside the 3.0625 r cull margin, including
    // the retained stretch, and the rounded square must be inscribed exactly in
    // the disk's support circle. A future shape that outgrows the bound fails here
    // instead of clipping silently at the frustum edge:
    //   max(support, kStretchCap * support) = 3.000 r <= 3.0625 r (2.08% headroom).
    //
    // Rejected variants, recorded so they cannot return unnoticed: a rounded
    // square with half-extent a = R has support 2.621 r, the unrounded square with
    // a = R has 2.828 r and the earlier 0.5-cell half-extent square has 2.507 r.
    // All three are larger than the adopted 2.0 r support, so they remain rejected
    // as appearance (a bigger silhouette and more fill); the raised margin no
    // longer makes them cull-infeasible, so they are recorded rather than gated.
    static_assert(
        gridParticleRoundedSquareSupport(kGridParticleSolidHalfExtentScale, kGridParticleSolidCornerScale) <=
            kGridParticleSupportScale + 1.0e-4f,
        "the solid rounded square is inscribed in the R support circle");
    static_assert(
        gridParticleRoundedSquareSupport(kGridParticleSolidHalfExtentScale, kGridParticleSolidCornerScale) *
                kGridParticleStretchCap <=
            kGridParticleCullRadiusScale,
        "the stretched support must fit the 3.0625 r cull margin");
    static_assert(
        gridParticleRoundedSquareSupport(
            kGridParticleSupportScale, kGridParticleSolidCornerFraction *kGridParticleSupportScale) >
            kGridParticleSupportScale,
        "the rejected rounded a = R variant (2.621 r) is larger than the adopted support; keep it rejected");
    static_assert(
        1.41421356237309505f * kGridParticleSupportScale > kGridParticleSupportScale,
        "the rejected unrounded a = R variant (2.828 r) is larger than the adopted support; keep it rejected");
    static_assert(
        2.5066f > kGridParticleSupportScale,
        "the rejected 0.5-cell half-extent square (2.507 r) is larger than the adopted support; keep it rejected");

    // The tangent contour never leaves the legal band: its minor radius cannot cut
    // into the plateau (and cannot reach it either - the shader's warp divides by
    // the band offset, so equality is a zero denominator and a NaN alpha) and its
    // major radius cannot exceed the support circle. The shader carries the same
    // pair as statement-scope static_asserts in particleTangentFalloff.
    static_assert(
        kGridParticleTangentContourScale * (1.0f - 0.5f * kGridParticleTangentContourBias) >
            kGridParticleFalloffInnerFraction,
        "the tangent contour's minor radius must stay strictly above the plateau: the warp divides by the"
        " band offset");
    static_assert(
        kGridParticleTangentContourScale * (1.0f + 0.5f * kGridParticleTangentContourBias) <= 1.0f,
        "the tangent contour's major radius must stay inside the support radius R");
    // The defensive clamp sits inside the same band, so it can only raise the
    // minor endpoint and can never move the major one.
    static_assert(
        kGridParticleTangentContourMinimum > kGridParticleFalloffInnerFraction and
            kGridParticleTangentContourMinimum <= kGridParticleTangentContourScale and
            kGridParticleTangentContourScale * (1.0f + 0.5f * kGridParticleTangentContourBias) <= 1.0f,
        "the defensive contour clamp must stay inside the legal band");
    // The energy gate only scales the bias down: strength is 0 below the low edge
    // and 1 above the high edge, so the maximum-bias proof above still bounds
    // every b_eff the fragment can use.
    static_assert(
        kGridParticleTangentConfidenceLow > 0.0f and
            kGridParticleTangentConfidenceLow < kGridParticleTangentConfidenceHigh,
        "the tangent energy gate needs a positive, increasing strength band");
    // The join closure must exceed the binary32 ulp at rho = 1 (so a sample built
    // exactly at R is closed) and stay invisible (well under a thousandth of R).
    static_assert(
        kGridParticleTangentJoinClosure > 4.0f * 1.1920929e-7f,
        "the tangent join closure must exceed the binary32 ulp at the support radius");
    static_assert(
        kGridParticleTangentJoinClosure < 1.0e-3f,
        "the tangent join closure must stay far below any visible radius change");

    // ------------------------------------------------------------------
    // particle publication state
    // ------------------------------------------------------------------

    // Three-state contract of one frame's particle publication (plan §3, Task
    // 5). It is shared by the packet, the staged frame and this pass so the
    // layers cannot disagree about who owns liveness:
    //   seeds   - no committed view and no attached matter: the pre-runtime CPU
    //             admission path; carries CPU batches and no GPU view.
    //   current - a committed view exists and `GridParticleGpuView::current()`
    //             is true: pack from the solver rows and draw.
    //   held    - matter is attached and a committed view exists, but its live
    //             epoch has moved on: the packet carries no view and no buffer at
    //             all, and the renderer presents its own retained packed rows.
    // Matter attached with *no* view is not a state: it is a broken source and
    // stays `state_not_recoverable` at capture.
    enum class EParticlePresentation : u8 {
        seeds,
        current,
        held
    };

    // ------------------------------------------------------------------
    // per-instance particle descriptions
    // ------------------------------------------------------------------
    // Draw class of one instanced particle draw. It is a per-draw depth/blend
    // state, not a description family: gas keeps the mask disk lane, and the
    // other two both composite their edge coverage premultiplied and differ only
    // in depth ownership and in whether authored opacity participates (optical)
    // or not (opaque). The material's own phase flags choose the description,
    // independently of this class.
    enum class EGridParticleDrawClass : u32 {
        gas = 0u,
        opaque = 1u,
        optical = 2u
    };

    // Phase of a particle material, read from the authored surface flags of the
    // instance's own material. An unauthored phase keeps the solid description.
    enum class EGridParticlePhase : u8 {
        solid,
        liquid
    };

    // One per-instance presentation description. m_visual borrows the palette
    // record for the row's own material id; it is never entry 0 or the resolved
    // destination cell's material.
    struct GridParticleDescription {
        const GridTerrainVisual *m_visual{};
        EGridParticlePhase m_phase{EGridParticlePhase::solid};
        // Canonical packCell word rebuilt from the row: material | temperature << 16.
        u32 m_canonical_word{};
    };

    enum class EGridParticleDescriptionReject : u8 {
        none,
        unknown_material,
        phase_mismatch,
    };

    struct GridParticleDescriptionSelection {
        GridParticleDescription m_description{};
        EGridParticleDescriptionReject m_reject{EGridParticleDescriptionReject::none};
        [[nodiscard]] bool selected() const noexcept {
            return m_reject == EGridParticleDescriptionReject::none;
        }
    };

    // The palette record for a row's own material, or nullptr when the palette
    // does not carry that id (finding D9). Never a fallback entry.
    [[nodiscard]] const GridTerrainVisual *particleMaterialVisual(
        std::span<const GridTerrainVisual> visuals, u32 material_flags_temperature) noexcept;

    // Nonvisual selection of the phase-appropriate description for one row.
    // The family is derived from the row's own surface flags, never from the
    // draw class, so an opaque liquid (liquid phase without the optical-blend
    // bit) still resolves to the liquid description and an optical solid to the
    // solid one. Asking for the other family is a reject, which is how the
    // "no liquid through the solid description" invariant is asserted.
    [[nodiscard]] GridParticleDescriptionSelection selectParticleDescription(
        std::span<const GridTerrainVisual> visuals, u32 material_flags_temperature, EGridParticlePhase phase) noexcept;

    // Derived eight-byte transport source for one admitted visible row. Linear
    // texture_mean is the existing filtered albedo mean; missing albedo passes
    // white. Receive-only transparent rows and the empty entry are all zero.
    [[nodiscard]] Expected<lighting::SourceMaterial> gridSourceMaterial(
        const GridTerrainVisual &visual, float3 texture_mean);

    // ------------------------------------------------------------------
    // coarse material projection
    // ------------------------------------------------------------------

    // One canonical coarsest admitted linear-image mean; no texture data is
    // retained.
    [[nodiscard]] Expected<std::array<float, 3u>> gridFilteredAlbedoMean(const image::ImageAsset &source);

    struct alignas(16) GridBackgroundLayer {
        u32 m_texture_index{0xffffffffu};
        u32 m_order{};
        u32 m_color_rgba{0xffffffffu};
        float m_repeat_period_m{256.0f};
        float m_parallax_factor{};
        u32 m_reserved[3]{};
    };
    static_assert(sizeof(GridBackgroundLayer) == 32u);
    static_assert(PPR_OFFSETOF(GridBackgroundLayer, m_repeat_period_m) == 12u);
    static_assert(PPR_OFFSETOF(GridBackgroundLayer, m_parallax_factor) == 16u);

    inline constexpr u32 kGridSurfaceColumns = 4096u;
    using GridSurfaceHeights = std::array<std::array<float, 4u>, kGridSurfaceColumns / 4u>;
    static_assert(sizeof(GridSurfaceHeights) == 16384u);
    // Exact generation height at each fine-column centre. Immutable owned
    // presentation data; no generation recipe or simulation dependency crosses here.
    struct GridSurfaceProfile {
        float m_cell_size_m{1.0f};
        GridSurfaceHeights m_heights_m{};
    };

    // Initial-state admission seeds in physical units, before a resident solver
    // view exists. Uploaded once per changed inventory; never CPU draw geometry.
    struct GridParticleInstance {
        u64 m_id{};
        double m_position_x_m{};
        double m_position_y_m{};
        double m_volume_m3{};
        double m_mass_kg{};
        double m_temperature_k{};
        u32 m_material_id{};
    };
    struct GridParticleBatch {
        u32 m_chunk_id{};
        std::shared_ptr<const Array<GridParticleInstance>> m_particles{};
    };

    // ------------------------------------------------------------------
    // versioned resident-particle presentation input
    // ------------------------------------------------------------------

    [[nodiscard]] Expected<std::shared_ptr<const GridParticleMetadataResource>> prepareGridParticleMetadata(
        rhi::IDevice &device, std::span<const GridParticleMetadataRow> rows, u32 moving_count, double cell_metres,
        u64 version);

    // ------------------------------------------------------------------
    // capture-only committed particle projection
    // ------------------------------------------------------------------

    // Capture-only all-row projection. Zero means a valid half-open native
    // cell; flags preserve invalid/outside/inactive/vacuum rows without culling.
    enum EGridParticleCensusStatus : u32 {
        kGridParticleCensusInvalid = 1u,
        kGridParticleCensusOutside = 2u,
        kGridParticleCensusInactive = 4u,
        kGridParticleCensusVacuum = 8u,
    };

    struct GridParticleCensusRow {
        u32 m_id_low{}, m_id_high{}, m_material{}, m_status{};
        u32 m_cell_x{0xffff'ffffu}, m_cell_y{0xffff'ffffu};
    };
    static_assert(sizeof(GridParticleCensusRow) == 24u and alignof(GridParticleCensusRow) == 4u);
    static_assert(
        std::is_standard_layout_v<GridParticleCensusRow> and std::is_trivially_copyable_v<GridParticleCensusRow>);
    static_assert(PPR_OFFSETOF(GridParticleCensusRow, m_cell_x) == 16u);

    struct GridParticleCensus {
        std::vector<GridParticleCensusRow> m_rows{};
        // Weak identity; the selected view/capture owns the metadata resource.
        std::weak_ptr<const GridParticleMetadataResource> m_metadata_owner{};
        GridParticlePresentationIdentity m_identity{};
        FenceCompletionValue m_submission{kFenceValueNone};
        u64 m_device_epoch{}, m_device_bytes{}, m_readback_bytes{}, m_cpu_bytes{};
        u32 m_world_edge{};
    };

    enum class EGridDiagnosticMode : u32 {
        textured,
        material,
        biome,
        temperature,
        initial_matter
    };
    struct alignas(16) GridDiagnosticColor {
        u32 m_color_rgba{};
        float m_fill_height_fine{4096.0f};
        u32 m_reserved[2]{};
    };
    static_assert(sizeof(GridDiagnosticColor) == 16u);
    static_assert(alignof(GridDiagnosticColor) == 16u);
    static_assert(PPR_OFFSETOF(GridDiagnosticColor, m_color_rgba) == 0u);
    static_assert(PPR_OFFSETOF(GridDiagnosticColor, m_fill_height_fine) == 4u);
    static_assert(PPR_OFFSETOF(GridDiagnosticColor, m_reserved) == 8u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_material_id) == 0u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_color_rgba) == 4u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_texture_index) == 8u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_texture_period_m) == 12u);

    // Frozen boundary struct (plain data): one chunk-sized tile submission.
    // m_chunk_id is the row-major chunk index (the sim chunkIndexOf domain,
    // carried here as a plain u32). m_dirty_mask bit i marks tile i dirty;
    // chunk-sized tiles use bit 0 only and every higher bit is rejected.
    // m_material_id is the element/material tint index.
    struct GridTileSubmission {
        u64 m_dirty_mask = 0u;
        GridTileRange m_tile_range{};
        u32 m_chunk_id = 0u;
        u32 m_material_id = 0u;
    };

    // Tile-quad GPU payload mirror. m_rect is min_x, min_y, max_x, max_y
    // in world-cell units on the z = 0 plane. Bit 0 selects the cell atlas;
    // otherwise the original flat m_material tint is unchanged.
    struct alignas(16) GridTilePayload {
        float m_rect[4]{};
        u32 m_material = 0u;
        u32 m_cell_word_base = 0u;
        u32 m_flags = 0u;
        u32 m_pad = 0u;
    };

    static_assert(std::is_trivially_copyable_v<GridTilePayload>);
    static_assert(std::is_standard_layout_v<GridTilePayload>);
    static_assert(sizeof(GridTilePayload) == 32u);
    static_assert(alignof(GridTilePayload) == 16u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_rect) == 0u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_material) == 16u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_cell_word_base) == 20u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_flags) == 24u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_pad) == 28u);

    // Frozen boundary struct (plain data): dirty-chunk tile upload. The view
    // is read-only and carries one payload per dirty tile in tile-index
    // order; only dirty tiles are uploaded, quiescent tiles are served from
    // the pass-owned cache.
    struct GridUploadRequest {
        u32 m_chunk_id = 0u;
        std::span<const GridTilePayload> m_tile_data_view{};
        // Optional, borrowed row-major 128x128 u16 material IDs. Empty means
        // flat tile; 0xffff denotes transparent. Copied before return.
        std::span<const u16> m_cell_materials{};
        // Optional, borrowed row-major 128x128 packed material+temperature
        // words for the full-domain terrain input. Empty retains the previous
        // packed cells; a temperature-only change uploads like a material one.
        std::span<const u32> m_cell_words{};
    };

    // One visible tile: the submission index it planned from plus the
    // resolved draw inputs. The plan preserves submission order.
    struct GridPlannedTile {
        u32 m_submission_index = 0u;
        u32 m_chunk_id = 0u;
        GridTileRange m_tile_range{};
        u32 m_material_id = 0u;
    };

    struct GridTilePlan {
        Array<GridPlannedTile> m_tiles{};
    };

    // Owner-lane snapshot. Counts describe encoded commands and retained terrain
    // resources, not GPU completion, driver allocation size or total VRAM.
    struct GridPassObservations {
        u64 m_draw_commands{};
        u64 m_dispatch_commands{};
        u64 m_atlas_uploaded_bytes{};
        u64 m_atlas_writes{};
        u64 m_atlas_serializations{};
        u64 m_atlas_deferrals{};
        u64 m_growth_waits{};
        u64 m_table_builds{};
        u64 m_table_uploads{};
        u64 m_retirement_steps{};
        u64 m_retired_chunk_blocks{};
        u64 m_texture_entries{};
        u64 m_texture_slots{};
        u64 m_texture_inventory_comparisons{};
        u64 m_texture_inventory_reuses{};
        // Successful new cache entries and their source mip bytes; excludes
        // dedup hits, descriptors, palettes and driver allocation overhead.
        u64 m_texture_uploads{};
        u64 m_texture_uploaded_bytes{};
        u64 m_candidate_advances{};
        u32 m_retained_atlases{};
        u64 m_particle_uploads{};
        u64 m_particle_uploaded_bytes{};
        u64 m_particle_instances{};
        u64 m_particle_buffer_bytes{};
        // Unique orientation allocations owned by this world: both transactional
        // roles plus any retained pre-growth history/draw source (4 B per capacity
        // row each). Included in m_particle_buffer_bytes as well, and reported
        // separately so the steady orientation share is visible.
        u64 m_particle_orientation_bytes{};
        // A metadata owner retained beside history when the renderer has not yet
        // replaced it (32 B per row); also included in m_particle_buffer_bytes.
        u64 m_particle_retained_metadata_bytes{};
        // Distinct published materials the palette does not carry. Those rows
        // discard at shading time (identity guard D9); nothing falls back to
        // palette entry 0. Counted once per successful pack.
        u64 m_particle_description_rejections{};
        u64 m_background_target_bytes{};
        u32 m_background_targets{};
        u32 m_particle_root_bindings{};
        u64 m_texture_decoded_bytes{};
    };

    // ------------------------------------------------------------------
    // staged frame
    // ------------------------------------------------------------------
    // Namespace scope, not nested GridPass types: a module interface or type
    // definition outside engine.app that names a nested type of an imported
    // class makes the MSVC front end unable to materialize unrelated class
    // definitions in that translation unit (C1001, function-signature.cpp:213).
    // GridPass keeps the established nested spellings as aliases.
    struct GridStagedUpload;

    struct GridStagedFrame;

    class GridPass {
    public:
        // Chunk-sized tiles: one tile per chunk over the 32x32-chunk grid of
        // 128x128-cell chunks (plain values here; no sim include).
        static constexpr u32 kTilesPerChunk{1u};
        static constexpr u32 kGridTileEdge{128u};
        static constexpr i32 kGridWorldEdge{4096};

        // Boundary rule: tile boxes are expanded by this epsilon (in
        // world-cell units) before the frustum test, so edge-touching tiles
        // are VISIBLE by construction while strictly-outside tiles stay
        // culled regardless of the frustum plane-test inclusivity.
        static constexpr float kGridCullEpsilon{0.5f};

        // Upload-cache capacity, clamped per bindless policy: one entry per
        // chunk of the whole world, bump-allocated with deterministic
        // fail-closed (no_buffer_space) overflow, never partial upload.
        static constexpr u32 kGridTileCacheCapacity{1024u};
        static constexpr u32 kCellsPerChunk{kGridTileEdge * kGridTileEdge};
        static constexpr u32 kCellWordsPerChunk{kCellsPerChunk / 2u};
        static constexpr u32 kCellAtlasSlots{2u * kGridTileCacheCapacity};
        static constexpr u32 kCellAtlasWords{kCellAtlasSlots * kCellWordsPerChunk};
        // Dense material palette capacity; the visible and derived source
        // palettes share this one sizing rule.
        static constexpr u32 kGridMaterialPaletteCapacity{65536u};

        // Indirect-lane vertex count: 6 non-indexed verts per tile quad, the
        // same geometry the CPU-staged drawInstanced path draws.
        static constexpr u32 kGridTileVertexCount{6u};

        // Per-frame raster view-projection and visibility lookup mode. The
        // render owner may supply temporal jitter; it never enters pipeline keys.
        struct GridFrame {
            float4x4 m_view_projection{identity_v};
            u32 m_use_visible_ids{};
            float m_cell_size_m{1.0f};
            u32 m_surface_mode{};
            u32 m_boundary_material_id{1u};
            float4x4 m_inverse_view_projection{identity_v};
            float m_viewport[4]{};
            float m_camera_origin[2]{};
            u32 m_has_background{};
            u32 m_has_surface{};
            std::array<GridBackgroundLayer, 4u> m_layers{};
            float m_camera_world_x{};
            float m_camera_world_y{};
            float m_camera_world_z{};
            float m_camera_forward_x{};
            float m_camera_forward_y{};
            float m_camera_forward_z{};
            u32 m_camera_ortho{};
            // Availability word for the categorical publication, mirrored by
            // `g_grid_frame.m_has_resolved_cells` in grid_tiles.slang. Bit 0:
            // the resolved-cell and fine-CH publication is bound. Bit 1: the
            // committed per-cell winner grid (`g_best_row`) written by that same
            // projection attempt is bound, so the terrain fragment may retire
            // particle-owned cells and the particle draw may occlude. The
            // prepare pass's Q16-fill target reads that attempt's grids through
            // the same ownership authority and carries its own bit in the
            // prepare constants. Bit 1 is never set without bit 0 and never while
            // the zero fallback is bound.
            u32 m_has_resolved_cells{};
        };

        // Shared two-bit availability word, mirrored by the shader's
        // `m_has_resolved_cells` field. Bit 1 requires the same attempt's grids:
        // it is derived from the actual committed-grid binding outcome.
        static constexpr u32 kFrameHasResolvedCells{1u};
        static constexpr u32 kFrameHasParticleOwnership{2u};

        static_assert(sizeof(GridFrame) == 336u);
        static_assert(alignof(GridFrame) == 16u);
        // Mango matrices have non-trivial copy/destruction; their established
        // row-major wire representation is pinned by these size/offset checks.
        static_assert(PPR_OFFSETOF(GridFrame, m_inverse_view_projection) == 80u);
        static_assert(PPR_OFFSETOF(GridFrame, m_viewport) == 144u);
        static_assert(PPR_OFFSETOF(GridFrame, m_layers) == 176u);
        static_assert(PPR_OFFSETOF(GridFrame, m_has_surface) == 172u);
        static_assert(PPR_OFFSETOF(GridFrame, m_camera_world_x) == 304u);
        static_assert(PPR_OFFSETOF(GridFrame, m_camera_forward_x) == 316u);
        static_assert(PPR_OFFSETOF(GridFrame, m_camera_ortho) == 328u);
        static_assert(PPR_OFFSETOF(GridFrame, m_has_resolved_cells) == 332u);

        // Background prerequisites (seeds, visuals/palette, camera/viewport,
        // ring slot) separate from the cached-backdrop encoding. The Editor
        // arms world lighting between them so the cache samples this attempt's
        // fine CH; legacy callers invoke both halves in order.
        [[nodiscard]] std::error_code prepareBackgroundInputs(
            const PreRenderContext &context, const CameraSnapshot &camera, const rhi::Viewport &viewport,
            const rhi::ScissorRect &scissor);
        [[nodiscard]] std::error_code prepareBackground(
            const PreRenderContext &context, const CameraSnapshot &camera, const rhi::Viewport &viewport,
            const rhi::ScissorRect &scissor);
        [[nodiscard]] std::error_code renderBackground(const DrawContext &context);

        // Restored instanced particle classes in plan order: the opaque
        // (depth-writing) class, then the optical (premultiplied) class. Both
        // draw the same ids and indirect arguments as the gas lane, and each
        // instance shades from its own material's phase flags — the class selects
        // only depth/blend, so an opaque liquid still takes the liquid
        // description. Terrain opaque must be drawn after this and before
        // renderGasParticles.
        [[nodiscard]] std::error_code renderSolidLiquidParticles(const DrawContext &context);

        // The gas lane at its existing slot in the scene order. Its vertex entry
        // explicitly rejects every moving row; the retired fragment discard used
        // to do that.
        [[nodiscard]] std::error_code renderGasParticles(const DrawContext &context);

        // ------------------------------------------------------------------
        // screen-space particle density (one admitted liquid lane)
        // ------------------------------------------------------------------
        // Increment B of the screen-space density plan: a GridPass-owned,
        // completion-fenced 1x (N, D[, O]) accumulation over the same prepared
        // rows and indirect arguments the direct classes use, and one
        // per-scene-sample composite at the lane's existing depth class slot.
        //
        // The lane is derived from the admitted palette and the published
        // metadata: exactly one material that the palette marks as a liquid
        // phase is admitted; zero admits nothing, and more than one is an
        // incompatible admission that keeps every particle on its existing
        // direct class rather than mixing materials in one target. The density
        // pass has no depth attachment, no history, no dirty tiles and no
        // partial clear; it clears to exact zero every rendered frame.
        [[nodiscard]] std::error_code prepareParticleSurface(
            const PreRenderContext &context, const CameraSnapshot &camera, const rhi::Viewport &viewport,
            const rhi::ScissorRect &scissor);
        [[nodiscard]] std::error_code renderParticleSurface(
            const DrawContext &context, EGridParticleDrawClass draw_class);
        // True when this frame admitted a lane and its density target was
        // encoded. The caller composites only at the matching class slot.
        [[nodiscard]] bool particleSurfaceAdmitted() const noexcept {
            return m_world.m_particle_surface.admitted();
        }
        [[nodiscard]] EGridParticleDrawClass particleSurfaceDrawClass() const noexcept {
            return m_world.m_particle_surface.m_draw_class;
        }

        // Tests-only: encodes both direct restored classes at their normal slots
        // without the density composite, so a fixture can prove the admitted lane
        // material's rows emit no direct instances while the lane is accumulated.
        [[nodiscard]] std::error_code renderDirectParticleClassesForTest(const DrawContext &context) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderGpuParticleClass_(context, EGridParticleDrawClass::opaque));
            return renderGpuParticleClass_(context, EGridParticleDrawClass::optical);
        }

        // Test-only density target readback after the caller proves completion.
        // No production path reads these textures on the CPU.
        struct ParticleSurfaceTargetsForTest {
            rhi::ITexture *m_density{};
            rhi::ITexture *m_optical{};
            u32 m_material{0xffff'ffffu};
            EGridParticleDrawClass m_draw_class{EGridParticleDrawClass::opaque};
            float m_pixel_world{};
        };
        [[nodiscard]] ParticleSurfaceTargetsForTest particleSurfaceTargetsForTest() const noexcept {
            const ParticleSurfaceSlot &slot = m_particle_surface_slots[m_particle_surface_slot];
            return {
                slot.m_density.get(), slot.m_optical.get(), m_world.m_particle_surface.m_material,
                m_world.m_particle_surface.m_draw_class, m_particle_surface_pixel_world};
        }

        // Convenience for single-pass callers (capture and fixtures): all three
        // classes in the same order, gas last.
        [[nodiscard]] std::error_code renderParticles(const DrawContext &context);

        // A host selects this view only after joint CPU/GPU commit. Old packets
        // are rejected on the owner lane before any upload/encoding touches it.
        //
        // "Current" is the frame's right to proceed, not the view's liveness: no
        // offered view means the frame carries seeds or a hold, neither of which
        // reads solver storage, so neither rejects the frame. An offered view
        // must belong to this world; `applyGpuParticleView` maps a not-current
        // view to the hold skip instead of failing (findings D10-D12).
        [[nodiscard]] bool gpuParticleFrameCurrent(const std::optional<GridParticleGpuView> &view) const noexcept;
        [[nodiscard]] std::optional<GridParticlePresentationIdentity> activeParticlePresentation() const noexcept;
        [[nodiscard]] std::error_code applyGpuParticleView(const GridParticleGpuView &view);
        [[nodiscard]] std::error_code prepareGpuParticles(const PreRenderContext &context, u32 census_world_edge = 0u);
        [[nodiscard]] Expected<GridParticleCensus> readParticleCensus(
            FenceCompletionValue completed, FenceCompletionValue capture_submission);

        // Strong immutable publication owners for the world-lighting consumer.
        // The visible/source palettes and full-domain terrain buffer are built
        // together from one admitted catalog version; the thermal table is the
        // pass device's single uploaded copy. The pass never resolves a hidden
        // service for these owners.
        [[nodiscard]] Expected<lighting::WorldLightingInputs> lightingInputs() const;

        // Normal (non-logging) availability query for production branching:
        // true when the admitted world supplies every publication owner world
        // lighting consumes. A world that has not published its catalog yet
        // keeps the documented flat fallback and prepares no lighting.
        [[nodiscard]] bool lightingInputsAvailable() const noexcept {
            return m_thermal_lut and
                   m_world.m_terrain_cells and
                   m_world.m_terrain_palette and
                   m_world.m_source_materials;
        }

        // ------------------------------------------------------------------
        // world-lighting owner
        // ------------------------------------------------------------------
        // The projection owner lives in the active WorldResources value with
        // its resolved cells, scratch and status tickets. These operations
        // delegate to it on the owner lane; no renderer or global service owns
        // world-lighting state.

        [[nodiscard]] std::error_code prepareWorldLighting(
            const PreRenderContext &context, const WorldLightingFrame &frame);
        [[nodiscard]] std::error_code finalizeWorldLighting(const PreRenderContext &context);
        [[nodiscard]] std::error_code bindWorldLighting(rhi::ShaderCursor &cursor) const;
        // C-stage retained inspection overlay. The active world owns the
        // published data; the caller's submission fence extends its readers
        // through notifyWorldLightingReadSubmitted.
        [[nodiscard]] bool inspectionAvailable() const noexcept;
        [[nodiscard]] std::error_code renderWorldLightingInspection(
            const DrawContext &context, const CameraSnapshot &camera, const LightingInspectionSettings &settings);
        void notifyWorldLightingSubmitted(FenceCompletionValue value) noexcept;
        void notifyWorldLightingReadSubmitted(FenceCompletionValue value) noexcept;
        void discardWorldLightingUnsubmitted() noexcept;
        // Settles a definitely-unsubmitted particle preparation: clears the pending
        // pack and orientation tokens and restores the accepted draw source. The
        // editor's definite pre-submit rejection calls this beside the lighting
        // discard; the ambiguous-submission quarantine is untouched.
        void discardUnsubmittedParticles() noexcept;
        [[nodiscard]] std::error_code collectWorldLightingCompleted(FenceCompletionValue completed);
        [[nodiscard]] WorldLightingPass *activeWorldLightingForTest() const noexcept {
            return m_world.m_lighting.get();
        }

        // Pure CPU planner: keeps the submissions whose tile boxes (expanded
        // by kGridCullEpsilon) intersect the snapshot frustum. Renders
        // nothing, so it is unit-testable without a device.
        [[nodiscard]] static Expected<GridTilePlan> planTiles(
            const CameraSnapshot &snapshot, std::span<const GridTileSubmission> submissions, float cell_size_m = 1.0f);

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        // Explicit owned-frame sampling policy.
        [[nodiscard]] std::error_code update(TimeSpan dt, const CameraSnapshot &camera_view, bool spatial_msaa = false);
        [[nodiscard]] std::error_code validateSpatialContent() const noexcept;

        // CPU reference draw path: plan (cull) -> resolve payloads (cache hit, or
        // stage cold from the submission) -> upload tile ring -> one
        // instanced draw (6 verts per quad, startInstanceLocation 0,
        // g_tile_base uniform carries the payload base because
        // SV_InstanceID is draw-local). Failure is fail-closed but NOT an
        // atomic frame: planning and payload upload are all-or-nothing, while
        // a group that cannot encode stops the frame with prior draws
        // standing (single draw here, so the frame is either whole or empty).
        [[nodiscard]] std::error_code render(const DrawContext &draw_context);

        // Additive multi-draw-indirect lane beside render(): the CPU-staged
        // drawInstanced path above keeps working untouched. Compaction is
        // pure CPU (compactIndirect, unit-testable without a device); the
        // encode binds the same pipeline/payloads and issues one drawIndirect
        // over the CPU-compacted args. Retained as a reference lane.
        [[nodiscard]] static Expected<Array<rhi::IndirectDrawArguments>> compactIndirect(const GridTilePlan &plan);

        [[nodiscard]] std::error_code renderIndirect(const DrawContext &draw_context);

        // Production lane: reset/cull into per-slot IDs and one indirect draw
        // record before the render pass, then consume them without CPU readback.
        [[nodiscard]] std::error_code prepareGpuVisibility(const PreRenderContext &context);
        [[nodiscard]] std::error_code renderGpuIndirect(const DrawContext &context);

        // Translucent terrain second pass (Task 9). Draws the same chunk quad
        // geometry as renderGpuIndirect but discards opaque cells and uses a
        // depth-test / no-depth-write blended pipeline, ordered after the
        // particle/fluid pass so transparent cells composite over them.
        [[nodiscard]] std::error_code renderTranslucent(const DrawContext &context);

        // Test readback after the caller proves completion. Borrowed current-slot
        // buffers remain owned by this pass; production never reads them on CPU.
        [[nodiscard]] std::pair<rhi::IBuffer *, rhi::IBuffer *> visibilityBuffersForTest() const noexcept {
            const VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
            return {slot.m_args.get(), slot.m_ids.get()};
        }

        // The one device-lifetime immutable BRDF integration table. Readers keep
        // their strong references through the frame fence; teardown releases it
        // after renderer completion and before RHI shutdown.
        [[nodiscard]] rhi::ITexture *brdfLutForTest() const noexcept {
            return m_brdf_lut.get();
        }

        // Read-only owners used by material probes that exercise the real
        // palette/descriptor heap. They borrow the pass's active world.
        [[nodiscard]] rhi::IBuffer *terrainPaletteForTest() const noexcept {
            return m_world.m_terrain_palette.get();
        }
        [[nodiscard]] rhi::IBuffer *textureDescriptorsForTest() const noexcept {
            return m_world.m_texture_inventory and m_world.m_texture_inventory->m_cache
                       ? m_world.m_texture_inventory->m_cache->descriptorBuffer()
                       : nullptr;
        }
        [[nodiscard]] rhi::DescriptorHandle terrainSamplerHandleForTest() const noexcept {
            return m_terrain_sampler_handle;
        }

        // Tests-only: binds externally prepared categorical state (resolved
        // cells + decoded fine CH) to the real material/gas program without
        // driving the full cascade transport. The pass retains strong owners
        // until shutdown/device loss. Production never calls these.
        [[nodiscard]] std::error_code setLightingBuffersForTest(
            rhi::ComPtr<rhi::IBuffer> resolved, rhi::ComPtr<rhi::IBuffer> fine_ch);

        // Tests-only: runs the same terrain ownership authority the draw path
        // uses (bit derivation + `g_best_row` binding) and reports the
        // availability word the terrain frame would carry. `availability` bit 1
        // is set only when the current attempt's committed grid was actually
        // bound; the zero fallback leaves it clear.
        [[nodiscard]] std::error_code bindTerrainLightingForTest(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, u32 &availability);

        // Tests-only: true when the last terrain ownership authority call bound
        // the zero fallback instead of the current attempt's committed grid.
        [[nodiscard]] bool terrainOwnershipFallbackUsedForTest() const noexcept {
            return m_terrain_ownership_fallback_used;
        }

        // Test-only ownership observations for the terrain staging lifetime.
        [[nodiscard]] rhi::IBuffer *terrainStagingForTest() const noexcept {
            return m_world.m_terrain_staging.get();
        }
        // Tests-only: the zero resolved-cell/CH fallback bound when no current
        // field exists. It is created on first background/material binding.
        [[nodiscard]] rhi::IBuffer *lightingFallbackForTest() const noexcept {
            return m_lighting_fallback.get();
        }
        [[nodiscard]] u32 pendingDeferredReleaseCountForTest() const noexcept {
            return m_deferred_releases.pendingCount();
        }

        [[nodiscard]] std::array<rhi::IBuffer *, 3u> particleBuffersForTest() const noexcept {
            if (not m_world.m_gpu_particles) {
                return {};
            }
            const auto &particles = *m_world.m_gpu_particles;
            const auto &slot = particles.m_slots[particles.m_selected_slot];
            return {particles.m_packed.get(), slot.m_args.get(), slot.m_ids.get()};
        }

        // Test-only orientation history roles: {accepted, candidate}. The accepted
        // role is what the next update slews from and what a repeated publication
        // binds; the candidate is this publication's uncommitted write. Both are
        // null before the first prepared publication.
        [[nodiscard]] std::array<rhi::IBuffer *, 2u> particleOrientationRolesForTest() const noexcept {
            if (not m_world.m_gpu_particles) {
                return {};
            }
            const auto &particles = *m_world.m_gpu_particles;
            return {particles.m_orientation[0u].get(), particles.m_orientation[1u].get()};
        }
        [[nodiscard]] u32 particleOrientationAcceptedRoleForTest() const noexcept {
            return m_world.m_gpu_particles ? m_world.m_gpu_particles->m_orientation_accepted_role : 0u;
        }
        [[nodiscard]] bool particleOrientationValidForTest() const noexcept {
            return m_world.m_gpu_particles ? m_world.m_gpu_particles->m_orientation_valid : false;
        }
        // Rows the accepted history covers (the metadata count at acceptance).
        [[nodiscard]] u32 particleOrientationCountForTest() const noexcept {
            return m_world.m_gpu_particles ? m_world.m_gpu_particles->m_orientation_count : 0u;
        }
        // The buffer this frame's particle draw reads: the candidate when the frame
        // encoded an update, otherwise the accepted role or the retained history.
        [[nodiscard]] rhi::IBuffer *particleOrientationDrawForTest() const noexcept {
            return m_world.m_gpu_particles ? m_world.m_gpu_particles->m_orientation_draw.get() : nullptr;
        }
        // The complete accepted history between publications: roles[accepted] when
        // that role is valid, otherwise the retained pre-growth allocation.
        [[nodiscard]] rhi::IBuffer *particleOrientationAcceptedForTest() const noexcept {
            if (not m_world.m_gpu_particles) {
                return nullptr;
            }
            const auto &particles = *m_world.m_gpu_particles;
            return particles.m_orientation_role_valid
                       ? particles.m_orientation[particles.m_orientation_accepted_role].get()
                       : particles.m_orientation_history.get();
        }
        [[nodiscard]] rhi::IBuffer *particleOrientationHistoryForTest() const noexcept {
            return m_world.m_gpu_particles ? m_world.m_gpu_particles->m_orientation_history.get() : nullptr;
        }

        // Test-only publication observations: the state the last applied stage
        // carried, the consecutive held preparations since the last current
        // publication, and the source epoch of the last completed pack.
        [[nodiscard]] EParticlePresentation particlePresentationForTest() const noexcept {
            return m_world.m_particle_presentation;
        }
        [[nodiscard]] u64 particleHoldAttemptsForTest() const noexcept {
            return m_world.m_particle_hold_attempts;
        }
        [[nodiscard]] u64 particlePackedEpochForTest() const noexcept {
            return m_world.m_gpu_particles ? m_world.m_gpu_particles->m_packed_epoch : std::numeric_limits<u64>::max();
        }

        // Pack dispatches encoded so far. A held preparation encodes none, so a
        // hold can never be confused with a re-pack by another lane's counters.
        [[nodiscard]] u64 particlePackEncodesForTest() const noexcept {
            return m_particle_pack_encodes;
        }

        // Test-only: the cached pipeline of one particle draw class, or nullptr
        // before that class first drew in this pass. Its reflected description
        // carries the declaration order and depth/blend state.
        [[nodiscard]] rhi::IRenderPipeline *particlePipelineForTest(EGridParticleDrawClass draw_class) const noexcept;

        // Diagnostic-only timestamp pairs: pack/cull, element splat, density
        // splat, filter, composite, and the particle draw interval (10 opens at
        // the first particle class, 11 closes after the last, so it spans the
        // opaque, optical and gas class draws). The caller permits one
        // instrumented frame in flight and resets/reads after its exact fence.
        [[nodiscard]] std::error_code setParticleTimingQueriesForTest(
            rhi::ComPtr<rhi::IQueryPool> queries, FenceCompletionValue completed);

        // Infallible teardown: clears staged submissions, tile cache, and
        // counters, then releases GPU objects; always returns success.
        [[nodiscard]] std::error_code shutdown();

        // Device-loss hook: drops GPU objects (pipeline root, payload ring),
        // parks uploads, and drops staged submissions while RETAINING the
        // tile cache and counters so resolveCached keeps serving while
        // GPU-touching calls fail closed. Restart is shutdown + initialize.
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        // Staged submissions (copied): fail-closed (invalid_argument) on a
        // malformed range or a reserved dirty-mask bit. Pure CPU: needs no
        // device, so device-free tests and the colony translator can stage
        // before (or without) initialize; only render touches the GPU.
        [[nodiscard]] std::error_code submitTiles(std::span<const GridTileSubmission> submissions);

        void clearTiles() noexcept;

        // Invalidates CPU tile/cell caches without discarding staged tiles.
        // Render-thread confined; keeps the GPU atlas alive until teardown,
        // and slot rewrites require completed reader-fence proof.
        void clearCache() noexcept;

        // Dirty-chunk upload lane: writes the request view into the
        // pass-owned cache for exactly the dirty tiles of the staged
        // submission, and returns the uploaded tile count. Quiescent (clean)
        // chunks upload nothing and keep serving from the cache. Pure CPU
        // like submitTiles: needs no device. Render-thread confined like
        // every other mutator.
        [[nodiscard]] Expected<u32> requestUpload(const GridUploadRequest &request);

        /// Staged upload row carried by a staged frame; the type itself lives at
        /// namespace scope (see below) and keeps the established nested spelling.
        using GridStagedUpload = ::pP::GridStagedUpload;

        /// Staged frame applied by this pass; the type itself lives at namespace
        /// scope (see below) and keeps the established nested spelling here.
        using GridStagedFrame = ::pP::GridStagedFrame;

        // Applies one owned staging payload on the pass's owner lane:
        // optional cache drop, then staged tiles, then dirty uploads (the
        // same validation the translator's synchronous path used). On an
        // upload failure the pass clears its staged tiles + cache and
        // returns the error; the client must full-refresh on recovery.
        [[nodiscard]] std::error_code applyStagedFrame(const GridStagedFrame &stage);

        // Interactive publication never waits. False retains the previous GPU
        // table and edit acknowledgments while a bounded cohort is prepared.
        [[nodiscard]] Expected<bool> tryPrepareCellUploads(
            rhi::IDevice &device, FenceCompletionValue completed, u32 chunk_budget = 8u);

        // Owner-lane candidate preparation retains the active world. Identity
        // must match at activation; at most active+candidate or active+retiring
        // resource bundles exist. Readiness alone never activates a candidate.
        [[nodiscard]] std::error_code stageWorldCandidate(
            u64 job_id, u64 world_generation, const GridStagedFrame &stage, bool uploads_complete = true);
        [[nodiscard]] std::error_code appendWorldCandidate(
            u64 job_id, u64 world_generation, std::span<const GridStagedUpload> uploads, bool final_batch);
        [[nodiscard]] Expected<bool> advanceWorldCandidate(
            rhi::IDevice &device, FenceCompletionValue completed, u32 chunk_budget = 8u);
        [[nodiscard]] std::error_code activateWorldCandidate(u64 job_id, u64 world_generation, u64 fingerprint);
        void cancelWorldCandidate(u64 job_id) noexcept;
        [[nodiscard]] bool worldCandidateAdmissionReady(FenceCompletionValue completed) noexcept;
        [[nodiscard]] bool worldCandidateMatches(u64 job_id, u64 world_generation, u64 fingerprint) const noexcept;
        [[nodiscard]] u64 worldGeneration() const noexcept {
            return m_world.m_world_generation;
        }

        [[nodiscard]] GridWorldIdentity activeWorldIdentity() const noexcept {
            return {m_world.m_job_id, m_world.m_world_generation, m_world.m_content_fingerprint};
        }

        [[nodiscard]] u32 retainedAtlasCountForTest() const noexcept;
        [[nodiscard]] GridPassObservations observations() const noexcept;
        [[nodiscard]] u64 atlasDeferredCount() const noexcept {
            return m_atlas_deferred_count;
        }
        [[nodiscard]] u64 atlasUploadedBytes() const noexcept {
            return m_atlas_uploaded_bytes;
        }

        // Cache probe: the cached payload for a live entry. Quiescent tiles
        // are served here without touching the GPU. Stale or never-uploaded
        // chunk → invalid_argument.
        [[nodiscard]] Expected<GridTilePayload> resolveCached(u32 chunk_id) noexcept;

        // P1 fence settlement: records the fence value signalled by the
        // frame's SUBMIT for the ring slot this frame rotated onto. Called
        // by the frame owner after a successful submit; never called when
        // the submit failed (the slot keeps its older value, which is
        // conservative and safe: unwritten-or-stale contents are always
        // overwritten before any later use).
        void notifyFrameSubmitted(FenceCompletionValue submit_value) noexcept;

        // The retired queue-idle preparation path leaves zero serializations.
        // Keep that historical telemetry query alongside actual writes/growth waits.
        [[nodiscard]] u64 atlasSerializationCount() const noexcept {
            return 0u;
        }
        [[nodiscard]] u64 atlasWriteCount() const noexcept {
            return m_atlas_write_count;
        }

        [[nodiscard]] u64 residentTableBuildCount() const noexcept {
            return m_resident_table_build_count;
        }
        [[nodiscard]] u64 residentTableUploadCount() const noexcept {
            return m_resident_table_upload_count;
        }
        [[nodiscard]] u64 growthWaitCount() const noexcept {
            return m_growth_wait_count;
        }
        // Last-use completion per ring slot (shared cursor: one array covers
        // the payload ring and the indirect-args ring together).
        [[nodiscard]] std::array<FenceCompletionValue, 3u> slotCompletionValues() const noexcept {
            return m_tile_slot_values;
        }

        [[nodiscard]] u64 uploadCount() const noexcept {
            return m_upload_count;
        }
        [[nodiscard]] u64 cacheHitCount() const noexcept {
            return m_cache_hit_count;
        }
        [[nodiscard]] u32 stagedTileCount() const noexcept {
            return safe_narrowing(m_world.m_submitted_tiles.size());
        }
        [[nodiscard]] u32 cachedTileCount() const noexcept {
            return safe_narrowing(m_world.m_tile_cache.size());
        }

    private:
        // ------------------------------------------------------------------
        // validation and planning
        // ------------------------------------------------------------------

        [[nodiscard]] static std::error_code validateSubmission_(const GridTileSubmission &submission) noexcept;

        struct GridCachedPipeline;

        // ------------------------------------------------------------------
        // resolve, upload, and encode
        // ------------------------------------------------------------------

        // Payload-ring depth: 3 slots > 2 frame credits, so the selected
        // slot is complete without waiting whenever submissions retired in
        // order; the wait fallback covers the rest. Declared before the
        // pipeline entry below, whose per-slot roots are sized by it.
        static constexpr u32 kTileRingSize{3u};
        static_assert(kTileRingSize == kFrameRingSlots);

        // Dense material palette capacity and terrain upload staging bound.
        // Both the visible palette and the derived source palette use the same
        // size rule; chunk uploads are bounded like the existing cell cohort.
        static constexpr u32 kTerrainUploadChunks{8u};

        [[nodiscard]] static constexpr u32 materialPaletteSize(const bool has_visuals) noexcept {
            return has_visuals ? kGridMaterialPaletteCapacity : 1u;
        }

        std::error_code createShaderProgram_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        std::error_code createVisibilityPrograms_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        std::error_code ensureVisibilityBuffers_(rhi::IDevice &device);
        std::error_code ensureSurfaceBuffers_(rhi::IDevice &device);
        [[nodiscard]] std::error_code bindMaterialBuffers_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, bool retained_table, u32 &availability);
        std::error_code bindSurfaceBuffers_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, bool retained_table, u32 &availability,
            GridCachedPipeline *pipeline = nullptr, std::optional<u32> owner_slot = {});
        [[nodiscard]] Expected<bool> prepareTerrainVisuals_(rhi::IDevice &device, u32 image_budget);
        [[nodiscard]] std::error_code ensureTerrainSampler_(rhi::IDevice &device);
        [[nodiscard]] std::error_code stageDiagnostic_(const GridStagedFrame &stage);
        [[nodiscard]] std::error_code appendDiagnostic_(const GridStagedUpload &upload);
        [[nodiscard]] Expected<bool> prepareDiagnostic_(rhi::IDevice &device, u32 chunk_budget);

        struct GridCachedPipeline {
            rhi::ComPtr<rhi::IRenderPipeline> m_pipeline{};
            // Per-slot ROOT shader objects (one per ring slot), each with
            // its own resolved g_grid_frame cursor. The frame writes only
            // its selected slot's cursor and binds only that slot's root,
            // so no submission ever observes another frame's constants —
            // the persistent-root mutation pattern is gone. A signature
            // change defers the whole entry (pipeline + all roots) through
            // m_deferred_releases instead of dropping it mid-flight.
            std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize> m_slot_roots{};
            std::array<rhi::ShaderCursor, kTileRingSize> m_slot_frame_cursors{};
            std::array<std::vector<rhi::ComPtr<rhi::ITextureView>>, kTileRingSize> m_slot_backdrop_owners{};
            std::array<rhi::IBuffer *, kTileRingSize> m_slot_atlases{};
            std::array<FenceCompletionValue, kTileRingSize> m_slot_last_use{};
            std::array<u64, kTileRingSize> m_slot_world_generation{};
            std::array<u64, kTileRingSize> m_slot_world_job_id{};
            std::array<bool, kTileRingSize> m_slot_encoded{};
            std::array<rhi::IBuffer *, kTileRingSize> m_slot_particle_buffers{};
            std::array<rhi::ITextureView *, kTileRingSize> m_slot_backdrop_views{};
            std::array<bool, kTileRingSize> m_slot_stale_bindings{};
        };

        [[nodiscard]] std::error_code ensurePipelineRoots_(rhi::IDevice &device, GridCachedPipeline &pipeline);

        // Every signature-keyed render pipeline cache this pass owns. Update,
        // submission accounting, retirement and backdrop release walk this one
        // list so a new lane cannot be missed by one of them.
        [[nodiscard]] std::array<std::optional<GridCachedPipeline> *, 11u> cachedPipelines_() noexcept {
            return {
                &m_cached_pipeline, &m_particle_classes[0u].m_pipeline, &m_particle_classes[1u].m_pipeline,
                &m_particle_classes[2u].m_pipeline, &m_background_pipeline, &m_background_copy_pipeline,
                &m_background_spatial_pipeline, &m_particle_surface_accumulate_pipeline,
                &m_particle_surface_accumulate_optical_pipeline, &m_particle_surface_composite_opaque_pipeline,
                &m_particle_surface_composite_optical_pipeline};
        }
        [[nodiscard]] std::array<const std::optional<GridCachedPipeline> *, 11u> cachedPipelines_() const noexcept {
            return {
                &m_cached_pipeline, &m_particle_classes[0u].m_pipeline, &m_particle_classes[1u].m_pipeline,
                &m_particle_classes[2u].m_pipeline, &m_background_pipeline, &m_background_copy_pipeline,
                &m_background_spatial_pipeline, &m_particle_surface_accumulate_pipeline,
                &m_particle_surface_accumulate_optical_pipeline, &m_particle_surface_composite_opaque_pipeline,
                &m_particle_surface_composite_optical_pipeline};
        }

        struct ParticleChunk {
            std::shared_ptr<const Array<GridParticleInstance>> m_source{};
            bool m_dirty{};
        };

        // Each completed particle slot owns its completion-selected compaction
        // buffers and optional capture storage; gas disks share the one packed
        // source and draw resources.
        struct ParticleCensusStorage {
            rhi::ComPtr<rhi::IBuffer> m_projected{}, m_readback{};
            rhi::ComPtr<rhi::IShaderObject> m_root{};
            rhi::IDevice *m_device{}; // Actual allocator; device outlives pass slots.
            GridParticlePresentationIdentity m_identity{};
            FenceCompletionValue m_submission{kFenceValueNone};
            u64 m_device_epoch{};
            u32 m_count{}, m_world_edge{};
            bool m_encoded{};
        };

        struct GpuParticleSlot {
            rhi::ComPtr<rhi::IBuffer> m_ids{}, m_args{};
            std::optional<GridParticleGpuView> m_reader{};
            std::unique_ptr<ParticleCensusStorage> m_census{};
        };

        struct GpuParticleStorage {
            std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize>, 4u> m_roots{};
            std::array<GpuParticleSlot, kTileRingSize> m_slots{};
            rhi::ComPtr<rhi::IBuffer> m_packed{}, m_ranks{}, m_counts{}, m_prefix{};

            // Two transactional orientation roles, indexed by role, 4 B per
            // capacity row each. `m_orientation_history` is the complete accepted
            // history the next update reads and a hold draws; it usually aliases
            // one role, but after a capacity growth it is the retained old
            // allocation until the first post-growth candidate submission succeeds.
            // The fresh growth roles are never read before that acceptance.
            // `m_orientation_draw` is resolved per frame: the candidate when this
            // frame encoded an update, otherwise the accepted role or the retained
            // history.
            std::array<rhi::ComPtr<rhi::IBuffer>, 2u> m_orientation{};
            rhi::ComPtr<rhi::IBuffer> m_orientation_history{};
            rhi::ComPtr<rhi::IBuffer> m_orientation_draw{};
            std::shared_ptr<const GridParticleMetadataResource> m_orientation_metadata{};
            std::shared_ptr<const GridParticleMetadataResource> m_pending_orientation_metadata{};
            GridWorldIdentity m_orientation_world{};
            GridWorldIdentity m_pending_orientation_world{};
            // Accepted and pending presentation identities: the design's acceptance
            // clock (publication, terrain revision, joint tick) plus the world.
            GridParticlePresentationIdentity m_orientation_identity{};
            GridParticlePresentationIdentity m_pending_orientation_identity{};
            u64 m_orientation_version{};
            u64 m_pending_orientation_version{};
            u32 m_orientation_accepted_role{};
            u32 m_orientation_count{};
            u32 m_pending_orientation_count{};
            // True when roles[accepted_role] holds the complete accepted history;
            // false after growth until the first new candidate is accepted.
            bool m_orientation_role_valid{};
            bool m_orientation_valid{};
            bool m_orientation_pending{};

            GridParticleGpuView m_view{};
            std::shared_ptr<const std::atomic<u64>> m_packed_owner{}, m_pending_owner{};

            u64 m_packed_epoch{std::numeric_limits<u64>::max()}, m_packed_metadata{}, m_packed_terrain{};
            u64 m_pending_epoch{}, m_pending_metadata{}, m_pending_terrain{};
            u32 m_capacity{}, m_selected_slot{};
            bool m_pending_pack{}, m_prepared{}, m_slot_encoded{};
        };

        struct BackgroundSlot {
            rhi::ComPtr<rhi::ITexture> m_texture{};
            rhi::ComPtr<rhi::ITextureView> m_view{};
            FenceCompletionValue m_last_use{kFenceValueNone};
            u32 m_width{};
            u32 m_height{};
        };

        [[nodiscard]] std::error_code validateParticles_(const GridStagedFrame &stage) const;
        void stageParticles_(const GridStagedFrame &stage);
        void markPipelineUse_(GridCachedPipeline &pipeline, u32 slot) noexcept;
        [[nodiscard]] std::error_code admitParticleSeeds_(rhi::IDevice &device);
        // Liveness of the retained committed view and the world it belongs to.
        [[nodiscard]] bool retainedGpuParticleCurrent_() const noexcept;
        // The held preparation: no pack, no solver buffer read, and the retained
        // packed rows and stable IDs stay drawable. A never-packed hold presents no
        // particles. The per-frame detail is debug; a hold that persists past
        // `kParticleHoldWarnFrames` warns exactly once per episode.
        [[nodiscard]] std::error_code holdGpuParticles_();
        [[nodiscard]] std::error_code createGpuParticlePrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code ensureParticleMask_(rhi::IDevice &device);
        [[nodiscard]] std::error_code ensureGpuParticleStorage_(const PreRenderContext &context, u32 count);
        // Drops the accepted/candidate orientation roles, the retained history and
        // the accepted metadata owner. Replaced allocations are deferred through the
        // reader-fence owner. Called on world/reset lineage and device
        // discontinuity, never per frame.
        void resetParticleOrientation_() noexcept;
        // Clears bookkeeping for a particle attempt that was definitely not
        // submitted: the pending pack token, the pending orientation token and the
        // per-frame draw source. Called before any next-frame prepare early return
        // (including a hold) and on the editor's definite pre-submit rejection, so
        // a rejected candidate can never be drawn or committed later. An ambiguous
        // submission never reaches these paths; it stays quarantined.
        void settleParticleAttempt_() noexcept;
        [[nodiscard]] std::error_code renderGpuParticleClass_(
            const DrawContext &context, EGridParticleDrawClass draw_class);
        void prepareChunkSlotMap_(bool retained_table);
        [[nodiscard]] std::error_code createOpticalPrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        // Blend and depth state of one signature-keyed render pipeline. The
        // terrain and background lanes keep the depth-disabled defaults; the
        // restored particle classes declare their own.
        struct GridPipelineState {
            bool m_blend{false};
            bool m_premultiplied{false};
            bool m_depth_test{false};
            bool m_depth_write{false};
            const char *m_label{"grid optical content pipeline"};
        };

        // Every caller supplies the state explicitly: a `= {}` default would
        // need this nested type's default member initializers while GridPass is
        // still incomplete at this declaration. `reactive_program` names the MRT
        // entry point for an MRT signature; it is a program *slot* because the
        // program is created by ensureSpatialPrograms_ inside this call, after
        // the caller's arguments were evaluated.
        [[nodiscard]] Expected<GridCachedPipeline *> opticalPipeline_(
            rhi::IDevice &device, const RenderPipelineSignature &signature, rhi::IShaderProgram *program,
            std::optional<RenderPipelineKey> &key, std::optional<GridCachedPipeline> &cache, GridPipelineState state,
            rhi::ComPtr<rhi::IShaderProgram> *reactive_program = nullptr);
        // One of the three particle draw classes with the state in plan §3: gas
        // keeps its depth-disabled lane with the blend state its fragment entry
        // implies, opaque tests and writes depth with blending off, optical
        // tests depth without writing it and blends premultiplied.
        [[nodiscard]] Expected<GridCachedPipeline *> particlePipeline_(
            rhi::IDevice &device, const RenderPipelineSignature &signature, EGridParticleDrawClass draw_class);
        void fillOpticalFrame_(GridFrame &frame) const noexcept;
        // Ensures the zero resolved-cell/CH fallback the background and
        // material roots always declare. Idempotent; owns the creation so a
        // background-only path cannot reach a null buffer.
        [[nodiscard]] std::error_code ensureLightingFallback_(rhi::IDevice &device);
        // Keeps static g_best_row/g_cell_fill bindings valid on roots that declare
        // them when no current projection attempt owns the committed grids. The
        // shader's ownership bit is clear in that case, so they are never read.
        [[nodiscard]] std::error_code bindOwnershipFallback_(rhi::ShaderCursor &cursor) const;
        // Background frame tail: captured camera position, forward and
        // orthographic flag, plus the shared optical frame fields.
        void fillBackgroundFrame_(GridFrame &frame) const noexcept;
        // Binds the background receiver's fine CH input and reports whether the
        // field is live. With no current field the zero fallback is ensured and
        // bound, and the gate stays clear, so no unlit layer indexes it.
        [[nodiscard]] std::error_code bindBackgroundFineCH_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, u32 &has_resolved_cells);
        void clearOpticalResources_() noexcept;
        [[nodiscard]] std::error_code ensureSpatialPrograms_(rhi::IDevice &device, bool reactive = false);

        [[nodiscard]] std::error_code encodeTerrainIndirect_(
            const DrawContext &context, GridCachedPipeline &pipeline, rhi::IQueryPool *queries = nullptr,
            u32 first_query = 0u, u32 pass = 0u);

        [[nodiscard]] Expected<GridCachedPipeline *> pipelineFor_(
            rhi::IDevice &device, const RenderPipelineSignature &signature);

        // Translucent terrain pipeline: same program/geometry as pipelineFor_ but
        // depth-test with no depth write (the opaque pass already wrote depth).
        [[nodiscard]] Expected<GridCachedPipeline *> translucentPipelineFor_(
            rhi::IDevice &device, const RenderPipelineSignature &signature);

        // Binds the terrain pass selector into a shader cursor.
        [[nodiscard]] std::error_code bindGridPass_(
            rhi::ShaderCursor &cursor, u32 pass, EGridParticleDrawClass particle_class = EGridParticleDrawClass::gas);

        // One authority for the terrain availability word and the committed
        // ownership grids: bit 0 is the categorical publication, bit 1 is set only
        // when this call actually bound every committed grid the root declares (the
        // winner grid for base retirement and particle occlusion; a root that also
        // declares the Q16 fill, such as the particle prepare rank invocation,
        // binds that through the same authority). A stale attempt, a fixture
        // publication or no publication binds the zero fallbacks and leaves bit 1
        // clear.
        [[nodiscard]] std::error_code bindTerrainLighting_(rhi::ShaderCursor &cursor, u32 &availability);

        // Reclaims deferred pipelines/roots at or below `completed`.
        void reclaimCompleted_(FenceCompletionValue completed) noexcept;

        // Rotates onto a completion-selected slot and waits it out when
        // stale. Records m_last_used_slot for notifyFrameSubmitted.
        [[nodiscard]] std::error_code selectSlot_(FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor, u32 availability);

        [[nodiscard]] std::error_code ensureTilePayloads_(
            rhi::IDevice &device, u32 tile_count, FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code ensureIndirectArgs_(
            rhi::IDevice &device, u32 arg_count, FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code ensureCellBuffer_(rhi::IDevice &device, bool atlas_required);

        // Loads the generated thermal table and creates its single device copy.
        [[nodiscard]] std::error_code createThermalLut_(rhi::IDevice &device, const std::filesystem::path &content_dir);
        // Generates the device's one immutable 64x128 RG16F isotropic VNDF/Smith
        // BRDF integration table with the real shader entry and waits once at
        // initialization. It is never reallocated or rewritten per frame.
        [[nodiscard]] std::error_code createBrdfLut_(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);
        // Ensures the world-owned full-domain packed terrain buffer and its
        // completion-selected upload staging ring for `edge`. `recreate` forces
        // a zero-filled replacement for reset/recovery paths.
        [[nodiscard]] std::error_code ensureTerrainCells_(rhi::IDevice &device, u32 edge, bool recreate = false);
        // Encodes pending chunk copies into the current frame command buffer.
        [[nodiscard]] std::error_code flushTerrainCells_(const PreRenderContext &context);

        [[nodiscard]] std::error_code validateCellDraw_(const GridTilePlan &plan) const noexcept;

        void applyCellPayload_(GridTilePayload &payload, u32 chunk_id) const noexcept;

        [[nodiscard]] std::error_code rebuildResidentTable_();
        [[nodiscard]] Expected<bool> prepareCellCohort_(
            rhi::IDevice &device, std::span<const u32> chunks, FenceCompletionValue completed, u32 chunk_budget);
        void cancelCellCohort_() noexcept;

        // Drops the whole payload ring without touching a device: unmap only
        // runs when one is supplied (the growth/replacement path), while
        // shutdown and device loss release still-mapped buffers as-is.
        void releaseTilePayloads_(rhi::IDevice *device) noexcept;

        // Drops the whole indirect-args ring under the same contract as the
        // payload ring above. The shared ring cursor is owned by the payload
        // ring's release, so this one leaves it alone.
        void releaseIndirectArgs_(rhi::IDevice *device) noexcept;

        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        // The one render program: grid_tiles's tile-indexed vertex entry plus
        // the tint fragment entry. A signature change drops the cached
        // pipeline (and its persistent root object) alike.
        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_spatial_shader_program{};

        // Optional native MRT variants share the same fenced roots and caches.
        rhi::ComPtr<rhi::IShaderProgram> m_reactive_shader_program{}, m_reactive_background_program{};
        safe_ptr<IShaderService> m_shader_service{};
        std::filesystem::path m_content_dir{};
        rhi::IDevice *m_device{}; // Actual allocator; device outlives the pass.
        // Generated thermal lookup uploaded once per GridPass device.
        rhi::ComPtr<rhi::IBuffer> m_thermal_lut{};
        // One immutable 64x128 RG16F BRDF integration table per GridPass device,
        // plus its clamp/linear sampler. The one-shot generation program and
        // pipeline are released immediately after the boot dispatch.
        rhi::ComPtr<rhi::ITexture> m_brdf_lut{};
        rhi::ComPtr<rhi::ITextureView> m_brdf_lut_view{};
        rhi::ComPtr<rhi::ISampler> m_brdf_sampler{};
        // Zero-valued fallback for worlds whose lighting owner has not published
        // resolved cells/CH yet, so binding stays valid while the scene is black.
        rhi::ComPtr<rhi::IBuffer> m_lighting_fallback{};
        // Tests-only external categorical state; empty in production.
        rhi::ComPtr<rhi::IBuffer> m_test_resolved{};
        rhi::ComPtr<rhi::IBuffer> m_test_fine_ch{};
        struct VisibilityProgram {
            rhi::ComPtr<rhi::IShaderProgram> m_program{};
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize> m_roots{};
        };
        struct VisibilitySlot {
            rhi::ComPtr<rhi::IBuffer> m_ids{};
            rhi::ComPtr<rhi::IBuffer> m_args{};
        };
        std::array<VisibilitySlot, kTileRingSize> m_visibility_slots{};
        std::array<VisibilityProgram, 2u> m_visibility_programs{};

        struct GpuParticleProgram {
            rhi::ComPtr<rhi::IShaderProgram> m_program{};
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        };
        std::array<GpuParticleProgram, 4u> m_particle_prepare_programs{};
        GpuParticleProgram m_particle_census_program{};
        // Particle programs: the gas lane and the restored solid/liquid lane.
        // The opaque and optical draw classes share one solid/liquid program
        // because the phase and the class are read from the draw constants; only
        // their pipeline depth/blend state differs.
        rhi::ComPtr<rhi::IShaderProgram> m_gpu_particle_gas_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_gpu_particle_gas_spatial_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_reactive_particle_gas_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_gpu_particle_solid_liquid_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_gpu_particle_solid_liquid_spatial_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_reactive_particle_solid_liquid_program{};

        // One signature-keyed pipeline per particle draw class. All three draw
        // the same ids and indirect arguments and differ in program and state.
        struct ParticleClassCache {
            std::optional<RenderPipelineKey> m_key{};
            std::optional<GridCachedPipeline> m_pipeline{};
        };
        static_assert(static_cast<u32>(EGridParticleDrawClass::optical) + 1u == 3u);
        std::array<ParticleClassCache, 3u> m_particle_classes{};

        [[nodiscard]] ParticleClassCache &particleClass_(const EGridParticleDrawClass draw_class) noexcept {
            return m_particle_classes[static_cast<u32>(draw_class)];
        }
        [[nodiscard]] const ParticleClassCache &particleClass_(const EGridParticleDrawClass draw_class) const noexcept {
            return m_particle_classes[static_cast<u32>(draw_class)];
        }

        // The program of one particle draw class for the current sampling policy.
        [[nodiscard]] rhi::IShaderProgram *particleClassProgram_(EGridParticleDrawClass draw_class) const noexcept;
        // The MRT entry point slot of one class, resolved by the pipeline helper
        // after ensureSpatialPrograms_ has created it.
        [[nodiscard]] rhi::ComPtr<rhi::IShaderProgram> &particleClassReactiveProgram_(
            EGridParticleDrawClass draw_class) noexcept;

        // ------------------------------------------------------------------
        // screen-space particle density resources
        // ------------------------------------------------------------------

        // No lane material: every particle keeps its existing direct class.
        static constexpr u32 kParticleSurfaceNoMaterial{0xffff'ffffu};

        // The single admitted density lane. It is derived from the palette and
        // the published metadata, never from a user selector or a GPU readback,
        // and it lives with the world so a world exchange cannot inherit another
        // world's lane.
        struct ParticleSurfaceLane {
            u32 m_material{kParticleSurfaceNoMaterial};
            EGridParticleDrawClass m_draw_class{EGridParticleDrawClass::opaque};

            [[nodiscard]] bool admitted() const noexcept {
                return m_material != kParticleSurfaceNoMaterial;
            }
        };

        // One completion-selected density target set. Replaced only after this
        // slot's last reader completed; an in-flight target is never reused.
        struct ParticleSurfaceSlot {
            rhi::ComPtr<rhi::ITexture> m_density{};
            rhi::ComPtr<rhi::ITextureView> m_density_view{};
            rhi::ComPtr<rhi::ITexture> m_optical{};
            rhi::ComPtr<rhi::ITextureView> m_optical_view{};
            FenceCompletionValue m_last_use{kFenceValueNone};
            u32 m_width{}, m_height{};
        };

        // Derives the lane from the retained palette and metadata. Called once per
        // particle preparation, before any hold early return, so a held frame's
        // direct draws exclude the same material the density pass reconstructs.
        void deriveParticleSurfaceLane_() noexcept;
        // Ensures the slot's RGBA16F density target (and the R16F optical target
        // for an optical lane) exists at the requested raster size. The slot was
        // already proven complete, so replacement never touches an in-flight
        // target.
        [[nodiscard]] std::error_code ensureParticleSurfaceTarget_(
            rhi::IDevice &device, ParticleSurfaceSlot &slot, u32 width, u32 height);
        // One density pixel in world-plane metres at the camera's z = 0 plane.
        // The quad padding and the cull-plane guard both use it, so the raster
        // footprint and the visibility margin agree.
        [[nodiscard]] float particleSurfacePixelWorld_(const CameraSnapshot &camera) const noexcept;
        [[nodiscard]] std::error_code createParticleSurfacePrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code ensureParticleSurfaceSampler_(rhi::IDevice &device);
        // Narrow binder for the composite root: the frame's categorical resolution
        // word, the ownership authority's winner grid/availability bit, and the
        // palette identity guard. The composite program declares no
        // material/source/thermal field, so the full surface binder cannot be used.
        [[nodiscard]] std::error_code bindParticleSurfaceInputs_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, u32 &availability);
        // Ensures every shared owner the surface set can bind (contour/chunk-slot
        // maps, palette + source palette + texture inventory + boundary noise,
        // lighting fallback). Idempotent; the density pass never depends on the
        // scene pass having prepared these first.
        [[nodiscard]] std::error_code ensureParticleSurfaceOwners_(rhi::IDevice &device);
        // Binds the accumulation root's surface inputs. Every field is
        // validity-guarded (a program that dropped a dead resource is skipped
        // instead of dereferencing a null cursor) and every resource is ensured
        // first, so a missing input is a reported error, never a null binding.
        [[nodiscard]] std::error_code bindParticleSurfaceSplat_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, u32 &availability);
        [[nodiscard]] Expected<GridCachedPipeline *> particleSurfacePipeline_(
            rhi::IDevice &device, const RenderPipelineSignature &signature, rhi::IShaderProgram *program,
            std::optional<RenderPipelineKey> &key, std::optional<GridCachedPipeline> &cache, bool additive,
            bool depth_test, bool depth_write, const char *label);

        rhi::ComPtr<rhi::IShaderProgram> m_particle_surface_accumulate_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_particle_surface_accumulate_optical_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_particle_surface_composite_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_particle_surface_composite_optical_program{};
        // Linear filtering with an exact-zero border: density taps outside the
        // target contribute zero, never the edge texel.
        rhi::ComPtr<rhi::ISampler> m_particle_surface_sampler{};
        std::optional<RenderPipelineKey> m_particle_surface_accumulate_key{};
        std::optional<GridCachedPipeline> m_particle_surface_accumulate_pipeline{};
        std::optional<RenderPipelineKey> m_particle_surface_accumulate_optical_key{};
        std::optional<GridCachedPipeline> m_particle_surface_accumulate_optical_pipeline{};
        std::optional<RenderPipelineKey> m_particle_surface_composite_opaque_key{};
        std::optional<GridCachedPipeline> m_particle_surface_composite_opaque_pipeline{};
        std::optional<RenderPipelineKey> m_particle_surface_composite_optical_key{};
        std::optional<GridCachedPipeline> m_particle_surface_composite_optical_pipeline{};
        std::array<ParticleSurfaceSlot, kTileRingSize> m_particle_surface_slots{};
        // The slot this frame encoded. Recorded by notifyFrameSubmitted; a
        // definitely-unsubmitted frame never fences it.
        u32 m_particle_surface_slot{0u};
        float m_particle_surface_pixel_world{};
        bool m_particle_surface_encoded{};
        bool m_particle_surface_ready{};

        rhi::ComPtr<rhi::IQueryPool> m_particle_timing_queries{};
        bool m_visibility_prepared{false};
        // Last terrain ownership authority outcome: true when it bound the zero
        // fallback and therefore left the frame ownership bit clear. Tests-only
        // observation.
        bool m_terrain_ownership_fallback_used{true};


        rhi::ComPtr<rhi::IBuffer> m_contour_points{};
        rhi::ComPtr<rhi::IBuffer> m_contour_cases{};
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_chunk_slot_buffers{};
        std::array<void *, kTileRingSize> m_chunk_slot_mapped{};
        std::array<bool, kTileRingSize> m_chunk_slot_uploaded{};
        rhi::ComPtr<rhi::ITexture> m_particle_mask{};
        rhi::ComPtr<rhi::ITextureView> m_particle_mask_view{};
        rhi::ComPtr<rhi::ISampler> m_particle_mask_sampler{};
        rhi::ComPtr<rhi::ITexture> m_boundary_noise{};
        rhi::ComPtr<rhi::ITextureView> m_boundary_noise_view{};
        rhi::ComPtr<rhi::ITexture> m_terrain_fallback{};
        rhi::ComPtr<rhi::ITextureView> m_terrain_fallback_view{};
        rhi::ComPtr<rhi::ISampler> m_terrain_sampler{};
        rhi::DescriptorHandle m_terrain_fallback_handle{};
        rhi::DescriptorHandle m_terrain_sampler_handle{};


        std::array<bool, kTileRingSize> m_resident_table_uploaded{};
        u64 m_draw_commands{};
        u64 m_dispatch_commands{};
        u64 m_retirement_steps{};
        u64 m_retired_chunk_blocks{};
        u64 m_texture_inventory_comparisons{};
        u64 m_texture_inventory_reuses{};
        u64 m_texture_uploads{};
        u64 m_texture_uploaded_bytes{};
        u64 m_candidate_advances{};
        u64 m_resident_table_build_count{};
        u64 m_resident_table_upload_count{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        std::optional<GridCachedPipeline> m_cached_pipeline;
        std::optional<RenderPipelineKey> m_translucent_pipeline_key;
        std::optional<GridCachedPipeline> m_translucent_pipeline;

        // The payload ring: kTileRingSize upload buffers at GridTilePayload
        // (32 B) stride, each created and mapped ONCE and kept mapped for its
        // whole life. The frame selects its slot by per-slot completion
        // values (m_tile_slot_values), never by blind cursor parity: with 3
        // slots and at most 2 outstanding submissions the selected slot is
        // complete without waiting, and the wait fallback below covers the
        // rest (including artificially delayed completion). Pass-owned,
        // released in shutdown before waitOnHost.
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_tile_payloads{};
        std::array<void *, kTileRingSize> m_tile_payload_mapped{};
        u32 m_tile_ring_cursor{0u};
        u64 m_tile_payload_capacity = 0u;
        // Shared-cursor coverage, once: the indirect-args ring rotates with
        // the payload ring on the same cursor, so ONE completion array
        // (m_tile_slot_values) governs both. A submission references both
        // slots together; they retire together.
        std::array<FenceCompletionValue, kTileRingSize> m_tile_slot_values{};
        u32 m_last_used_slot{0u};

        // Retired pipelines/roots that in-flight submissions may still
        // reference, keyed by last-use fence value. Reclaimed at every
        // render entry from the frame's completion value; drained
        // unconditionally at shutdown/device-loss.
        DeferredReleaseQueue m_deferred_releases{};
        FenceCompletionValue m_last_submitted_value{kFenceValueNone};

        // Gate 1 telemetry (see accessors above).
        u64 m_atlas_write_count{0u};
        u64 m_growth_wait_count{0u};

        // The compacted MDI args ring: one IndirectDrawArguments per visible
        // tile, written by the CPU each submit. Same 3-slot rotation on the
        // shared cursor as the payload ring (governed by the same
        // m_tile_slot_values, covered once), so a frame's write never lands
        // in a buffer an in-flight frame is still reading.
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_indirect_args{};
        std::array<void *, kTileRingSize> m_indirect_args_mapped{};
        u64 m_indirect_args_capacity = 0u;

        // Bounded 64 MiB Upload atlas: 2048 physical slots for 1024 logical
        // chunks. Published and retained-table slots stay pinned; retired slots
        // are reusable only after their last reader fence completes. A 4-byte
        // dummy serves flat-only rendering without allocating the atlas.


        u64 m_atlas_deferred_count{};
        u64 m_atlas_uploaded_bytes{};

        struct GridCachedCells {
            std::unique_ptr<std::array<u32, kCellWordsPerChunk>> m_words{};
            Array<u32> m_cells{};
            u64 m_generation{1u};
            bool m_pending = true;
            bool m_full_pending = true;
        };


        // Pass-owned CPU tile cache + staged submissions. Render-thread
        // confined: no internal mutex; concurrent upload is a caller bug.
        struct GridCachedTile {
            GridTilePayload m_payload{};
            u32 m_material_id = 0u;
        };


        u64 m_upload_count = 0u;
        // Mutable so the const resolve probe can record hits; still
        // render-thread confined, never atomic.
        mutable u64 m_cache_hit_count = 0u;


        // ------------------------------------------------------------------
        // sealed ordered terrain texture inventory
        // ------------------------------------------------------------------

        // Private while uploading; shared only after sealing. The cache owns
        // descriptor storage and pinned GPU images, and these handles/slots
        // are meaningful only with that cache. No slots are added after seal.
        struct TerrainTextureInventory {
            Array<image::ImageAsset> m_images{};
            Array<u32> m_slots{};
            Array<TextureHandle> m_handles{};
            std::unique_ptr<BindlessTextureCache> m_cache{};
            rhi::IDevice *m_device{}; // Identity only; device outlives every world.

            u32 m_cursor{};
            bool m_sealed{};
            bool m_reuse_checked{};
        };

        // ------------------------------------------------------------------
        // independently published world resources
        // ------------------------------------------------------------------

        // One world owns its palette, atlas mappings and retained draw
        // state. Candidate preparation can exchange this value on the owner
        // lane without moving pass programs, completion rings or the camera.
        struct WorldResources {
            std::unique_ptr<GpuParticleStorage> m_gpu_particles{};
            std::shared_ptr<std::atomic<u64>> m_initial_particle_epoch{};
            u64 m_particle_seed_version{};
            // Publication state the last applied stage reported (plan §3). It is
            // world-scoped so a world swap cannot inherit another world's hold.
            EParticlePresentation m_particle_presentation{EParticlePresentation::seeds};
            // Consecutive held preparations since the last current publication.
            // While the solver runs this is normally a one-frame lag, so the
            // per-frame entry/exit is debug detail; a hold that persists past
            // `kParticleHoldWarnFrames` warns once and recovers with one info.
            // Expiry is a publication, a world event or matter detach (no timeout).
            u64 m_particle_hold_attempts{};
            // The one admitted density lane, derived from this world's palette and
            // the published metadata at every particle preparation.
            ParticleSurfaceLane m_particle_surface{};
            bool m_particle_seeds_dirty{};
            bool m_has_background{};
            std::array<GridBackgroundLayer, 4u> m_background_layers{};
            std::shared_ptr<const GridSurfaceProfile> m_surface_profile{};
            float m_reference_thickness_m{1.0f};
            std::array<ParticleChunk, kGridTileCacheCapacity> m_particles{};
            EGridDiagnosticMode m_diagnostic_mode{EGridDiagnosticMode::textured};
            u64 m_diagnostic_revision{};
            Array<GridDiagnosticColor> m_diagnostic_palette{};
            std::array<Array<u32>, kGridTileCacheCapacity> m_diagnostic_words{};
            u32 m_diagnostic_cursor{};
            bool m_initial_matter_valid{true};
            bool m_initial_matter_invalidation_pending{};
            rhi::ComPtr<rhi::IBuffer> m_diagnostic_atlas{};
            void *m_diagnostic_mapped{};
            rhi::ComPtr<rhi::IBuffer> m_diagnostic_colors{};
            bool m_candidate_uploads_complete{true};
            bool m_upload_submission_pending{false};
            bool m_visuals_set{false};
            Array<GridTerrainVisual> m_terrain_visuals{};
            std::shared_ptr<TerrainTextureInventory> m_texture_inventory{};

            u32 m_retirement_diagnostic_cursor{};
            u32 m_retirement_particle_cursor{};
            bool m_retirement_roots_detached{};
            rhi::ComPtr<rhi::IBuffer> m_terrain_palette{};
            // Derived eight-byte source palette, built with the visible palette
            // from the same admitted catalog version.
            rhi::ComPtr<rhi::IBuffer> m_source_materials{};
            // This world's categorical projection owner: resolved cells,
            // reusable winner scratch and numeric status tickets.
            std::unique_ptr<WorldLightingPass> m_lighting{};
            // World-owned full-domain packed terrain input (material16 + half Kelvin).
            // GPU copies from the fenced staging ring below target this buffer;
            // CPU code never writes GPU-readable device memory directly.
            rhi::ComPtr<rhi::IBuffer> m_terrain_cells{};
            rhi::ComPtr<rhi::IBuffer> m_terrain_staging{};
            void *m_terrain_staging_mapped{};
            std::array<FenceCompletionValue, kTileRingSize> m_terrain_staging_values{};
            Array<u32> m_terrain_encoded_chunks{};
            Array<u64> m_terrain_encoded_generations{};
            u32 m_terrain_staging_slot{};
            bool m_terrain_staging_encoded{};
            u32 m_world_edge{};
            u64 m_job_id{};
            u64 m_world_generation{};
            u64 m_content_fingerprint{};
            FenceCompletionValue m_last_reader{kFenceValueNone};
            bool m_prepared{false};
            bool m_visibility_has_cells{false};
            bool m_contours_enabled{false};
            bool m_resident_contours_enabled{false};
            float m_resident_cell_size_m{1.0f};
            u32 m_boundary_material_id{1u};
            u32 m_resident_boundary_material_id{1u};
            std::array<u32, kGridTileCacheCapacity> m_resident_chunk_slots{};
            float m_cell_size_m{1.0f};
            Array<GridTilePayload> m_resident_table{};
            bool m_resident_table_stale{true};
            rhi::ComPtr<rhi::IBuffer> m_cell_atlas{};
            void *m_cell_atlas_mapped = nullptr;
            rhi::ComPtr<rhi::IBuffer> m_cell_dummy{};
            std::array<std::optional<u32>, kGridTileCacheCapacity> m_cell_slots{};
            std::array<FenceCompletionValue, kCellAtlasSlots> m_cell_slot_values{};
            std::bitset<kCellAtlasSlots> m_cell_slots_current{};
            std::bitset<kCellAtlasSlots> m_resident_cell_slots{};
            std::bitset<kCellAtlasSlots> m_cell_slots_reserved{};
            Array<u32> m_cell_cohort_chunks{};
            Array<u32> m_cell_cohort_slots{};
            u32 m_cell_cohort_cursor{};
            bool m_cell_publication_deferred{false};
            FlatMap<u32, GridCachedCells> m_cell_cache{};
            FlatMap<u32, GridCachedTile> m_tile_cache{};
            Array<GridTileSubmission> m_submitted_tiles{};
        };
        WorldResources m_world{};
        std::unique_ptr<WorldResources> m_candidate_world{};
        std::unique_ptr<WorldResources> m_retiring_world{};

        CameraSnapshot m_camera_view;
        std::array<BackgroundSlot, kTileRingSize> m_background_slots{};
        u32 m_background_cursor{};
        bool m_background_ready{};
        bool m_surface_retained_table{};
        rhi::Viewport m_background_viewport{};
        rhi::ScissorRect m_background_scissor{};
        CameraSnapshot m_background_camera{};
        // Split background preparation: inputs admit prerequisites and select
        // the backdrop slot; the cache half encodes it after lighting.
        FenceCompletionValue m_background_inputs_completed{kFenceValueNone};
        bool m_background_needs_backdrop{};
        bool m_background_inputs_prepared{};
        rhi::ComPtr<rhi::IShaderProgram> m_background_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_background_copy_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_background_spatial_program{};
        std::optional<RenderPipelineKey> m_background_key{}, m_background_copy_key{};
        std::optional<GridCachedPipeline> m_background_pipeline{}, m_background_copy_pipeline{};
        std::optional<RenderPipelineKey> m_background_spatial_key{};
        std::optional<GridCachedPipeline> m_background_spatial_pipeline{};

        u64 m_particle_uploads{};
        u64 m_particle_uploaded_bytes{};
        // Pack dispatches encoded (one per frame whose source epoch, device epoch
        // or metadata version differs from the last completed pack). A held
        // preparation encodes none and must leave this unchanged.
        u64 m_particle_pack_encodes{};
        // Distinct published materials the palette does not carry; those rows
        // discard at shading time (identity guard D9). Counted once per pack.
        u64 m_particle_description_rejections{};
        bool m_cached_spatial_msaa{};
        bool m_spatial_msaa{};
        bool m_spatial_supported{};
        bool m_ready = false;
    };

    // ------------------------------------------------------------------
    // staged frame
    // ------------------------------------------------------------------

    /// One staged tile upload carried by a staged frame.
    struct GridStagedUpload {
        u32 m_chunk_id = 0u;
        GridTilePayload m_payload{};
        Array<u16> m_cell_materials{};
        // Row-major packed material+temperature words for the full-domain
        // terrain input; empty keeps the previous cells.
        Array<u32> m_cell_words{};
        Array<u16> m_diagnostic_indices{};
    };

    /// Owned per-frame staging payload, built on the CLIENT lane and applied on
    /// the pass's owner lane (encoded pass-affinity: the client never touches
    /// pass CPU state directly anymore). Plain values + owned storage only —
    /// never a live grid or pass view.
    struct GridStagedFrame {
        // Render the retained grid and actors while the client session is frozen.
        bool m_retain_content{false};
        // Static terrain can retain its checked cohort while the ordinary game
        // actor overlay advances; frozen-session callers keep the default false.
        bool m_update_actors_on_retained_terrain{};
        float m_cell_size_m{1.0f};
        // Full-domain edge for the packed terrain input. A stage that carries
        // m_cell_words must set this to the owning world edge; zero means no
        // full-domain input is published (per-chunk visible atlas only).
        u32 m_world_edge{};
        bool m_has_catalog{false};
        u32 m_boundary_material_id{1u};
        u64 m_content_fingerprint{};

        Array<GridTerrainVisual> m_terrain_visuals{};
        Array<image::ImageAsset> m_terrain_images{};
        bool m_has_background{false};
        std::array<GridBackgroundLayer, 4u> m_background_layers{};
        std::shared_ptr<const GridSurfaceProfile> m_surface_profile{};
        float m_reference_thickness_m{1.0f};
        Array<GridParticleBatch> m_particle_batches{};
        std::optional<GridParticleGpuView> m_gpu_particles{};
        // Publication state this stage carries (plan §3). `held` never carries a
        // view or a CPU batch: the renderer keeps presenting its own last pack.
        EParticlePresentation m_particle_presentation{EParticlePresentation::seeds};
        Array<GridTileSubmission> m_tiles{};
        Array<GridStagedUpload> m_uploads{};
        EGridDiagnosticMode m_diagnostic_mode{EGridDiagnosticMode::textured};
        u64 m_diagnostic_revision{};
        Array<GridDiagnosticColor> m_diagnostic_palette{};
        bool m_initial_matter_valid{true};
        // Dropped before staging: translator reset / regeneration / fail-closed
        // recovery. The client sets this; the lane applies the clear so
        // regeneration recovery never edits pass state from main.
        bool m_reset_caches = false;
        // The client's single armed full-refresh flag at capture: if this stage
        // exits on the non-applied path (rejected, minimized, cancelled,
        // absorb-and-drop, worker error), the derived host must re-arm its
        // full-refresh flag so the retry re-stages the world. The true value
        // clears it at settlement.
        bool m_full_refresh{false};
        // Consumption-ack manifest for the driver dirty-set covered by this
        // frame's staging payload: (seed, chunk ids, revisions) captured at the
        // capture point. The lane round-trips it into RenderFrameResult; the
        // derived owner consumes it there so the ack rides the reliable per-slot
        // settlement path (applied, not dropped, not errored) rather than staging.
        u64 m_ack_seed{};
        Array<u32> m_ack_chunk_ids{};
        Array<u64> m_ack_revisions{};
    };
}
