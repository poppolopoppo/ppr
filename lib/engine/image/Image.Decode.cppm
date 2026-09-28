module;
export module engine.image:decode;

import :types;
import engine.core;
import engine.math;

import std;

// ------------------------------------------------------------------
// image decoding interface
// ------------------------------------------------------------------

// Synchronous thread-safe decode (PNG/JPG/KTX2/DDS via Mango Bitmap).
// Mango stays PRIVATE to the matching .cpp —
// never exported here, so image -> rhi leaks are impossible at the boundary.

export namespace pP::image {
    // ------------------------------------------------------------------
    // rgba8 decoding
    // ------------------------------------------------------------------

    // Decodes any SUPPORTED source to RGBA8. sRGB follows the Mango header
    // (!header.linear); usage color trusts the header, usage data forces linear —
    // never from the filename. Y-flip (when requested) is a positive-pitch CPU
    // blit; uploads always emit positive-pitch rows in sampler order.
    [[nodiscard]] Expected<ImageAsset> decodeToRgba8(
        mem::SharedBufferView bytes, std::string_view ext, ImageDecodeDesc desc, EImageUsage usage);

    // ------------------------------------------------------------------
    // block decoding
    // ------------------------------------------------------------------

    // Passthrough/transcode of SUPPORTED compressed sources only (KTX2/DDS blocks
    // the Mango decoder exposes). NEVER recompresses PNG/JPG into blocks: those
    // return function_not_supported so the caller falls back to decodeToRgba8.
    [[nodiscard]] Expected<ImageAsset> decodeToBlocks(
        mem::SharedBufferView bytes, std::string_view ext, EBlockTag want, ImageDecodeDesc desc);
}
