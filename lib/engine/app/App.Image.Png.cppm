module;

#include "pP/Macros.h"

export module engine.app:image.png;

import :renderer;

import std;

// ------------------------------------------------------------------
// surface frame PNG encoding
// ------------------------------------------------------------------

export namespace pP {
    /// Writes one completed surface readback as an 8-bit RGBA PNG.
    /// Accepts the portable RGBA8/BGRA8 layouts produced by
    /// Renderer::completeSurfaceReadback_: the readback row pitch is honoured,
    /// BGRA is swizzled, and row 0 is emission row 0 (top-down, no vertical
    /// flip). Any unsupported format or inconsistent layout fails closed
    /// without leaving a file behind.
    [[nodiscard]] std::error_code writeSurfacePng(const SurfaceFramePixels &pixels, const std::filesystem::path &path);
} // namespace pP
