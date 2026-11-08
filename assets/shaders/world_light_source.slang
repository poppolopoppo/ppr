// ------------------------------------------------------------------
// dense world light source: native-cell ids, dense overlay, occupancy
// ------------------------------------------------------------------
// Shared scalar ABI for the world light source and its Matter overlay. Cell
// ids are one u32 per native cell (0 = vacuum). The dense overlay stores seven
// u32 words per cell (coverage, sigma[3], emission[3], each holding float bits)
// so the scatter can atomically accumulate float terms. Occupancy is one bitset
// level with one bit per 8x8 block, packed into u32 words with
// ceil(blocks_per_row / 32) words per block row.
#ifndef PPR_WORLD_LIGHT_SOURCE_SLANG
#define PPR_WORLD_LIGHT_SOURCE_SLANG

struct WorldLightSourceConstants {
    uint width, height, chunkEdge, occupancyWords;
    uint revision, hasOverlay;
    uint pad0, pad1;
};

// Global palette material, indexed by material id (0 = vacuum). m_albedo is the
// visual diffuse albedo used for the one-bounce previous-field feedback at
// opaque hits; it is zero for vacuum and overlay-only (particle) surfaces.
struct WorldLightMaterial {
    uint kind;
    float source[3];
    float sigma[3];
    float emission[3];
    float albedo[3];
};

// Per-chunk upload dispatch: the chunk coordinates this upload writes into the
// dense ids buffer.
struct WorldLightChunkUpload {
    uint chunkX, chunkY;
    uint pad0, pad1;
};

// Dense overlay word offsets within a cell's 7-word row.
static const uint kOverlayCoverage = 0u;
static const uint kOverlaySigma0 = 1u;
static const uint kOverlaySigma1 = 2u;
static const uint kOverlaySigma2 = 3u;
static const uint kOverlayEmission0 = 4u;
static const uint kOverlayEmission1 = 5u;
static const uint kOverlayEmission2 = 6u;
static const uint kOverlayStride = 7u;

ConstantBuffer<WorldLightSourceConstants> g_world_light_source;
ConstantBuffer<WorldLightChunkUpload> g_world_light_chunk_upload;
StructuredBuffer<uint> g_world_light_chunk;
RWStructuredBuffer<uint> g_world_light_ids;
RWStructuredBuffer<uint> g_world_light_overlay;
RWStructuredBuffer<uint> g_world_light_occupancy;

// Copies one staged 128x128 chunk (widened u32 ids) into the dense ids buffer,
// dropping out-of-world cells at the right/bottom edges.
[shader("compute")]
[numthreads(64, 1, 1)]
void uploadWorldLightChunk(uint3 id : SV_DispatchThreadID) {
    uint local = id.x;
    uint chunkEdge = g_world_light_source.chunkEdge;
    if (local >= chunkEdge * chunkEdge) return;

    uint lx = local % chunkEdge;
    uint ly = local / chunkEdge;
    uint gx = g_world_light_chunk_upload.chunkX * chunkEdge + lx;
    uint gy = g_world_light_chunk_upload.chunkY * chunkEdge + ly;

    uint width = g_world_light_source.width;
    uint height = g_world_light_source.height;
    if (gx >= width || gy >= height) return;

    g_world_light_ids[gy * width + gx] = g_world_light_chunk[local];
}

// Rebuilds the complete occupancy bitset from the dense ids and overlay. One
// thread owns one u32 word (a full block row strip of up to 32 blocks), so no
// two threads share a word. The occupied test mirrors the CPU oracle exactly:
// id != 0, or overlay coverage/sigma/emission strictly greater than zero.
[shader("compute")]
[numthreads(64, 1, 1)]
void buildWorldLightOccupancy(uint3 id : SV_DispatchThreadID) {
    uint word = id.x;
    uint width = g_world_light_source.width;
    uint height = g_world_light_source.height;
    uint wordsPerRow = g_world_light_source.occupancyWords;
    uint totalWords = ((height + 7u) / 8u) * wordsPerRow;
    if (word >= totalWords) return;

    uint blockRow = word / wordsPerRow;
    uint wordColumn = word % wordsPerRow;
    uint blocksX = (width + 7u) / 8u;

    uint result = 0u;
    uint blockStartX = wordColumn * 32u;
    for (uint b = 0u; b < 32u; ++b) {
        uint bx = blockStartX + b;
        if (bx >= blocksX) break;

        uint cx0 = bx * 8u;
        uint cy0 = blockRow * 8u;
        uint occupied = 0u;
        for (uint y = cy0; y < min(cy0 + 8u, height) && occupied == 0u; ++y) {
            for (uint x = cx0; x < min(cx0 + 8u, width); ++x) {
                uint index = y * width + x;
                if (g_world_light_ids[index] != 0u) {
                    occupied = 1u;
                    break;
                }

                uint base = index * kOverlayStride;
                if (asfloat(g_world_light_overlay[base + kOverlayCoverage]) > 0.0f ||
                    asfloat(g_world_light_overlay[base + kOverlaySigma0]) > 0.0f ||
                    asfloat(g_world_light_overlay[base + kOverlaySigma1]) > 0.0f ||
                    asfloat(g_world_light_overlay[base + kOverlaySigma2]) > 0.0f ||
                    asfloat(g_world_light_overlay[base + kOverlayEmission0]) > 0.0f ||
                    asfloat(g_world_light_overlay[base + kOverlayEmission1]) > 0.0f ||
                    asfloat(g_world_light_overlay[base + kOverlayEmission2]) > 0.0f) {
                    occupied = 1u;
                    break;
                }
            }
        }
        if (occupied != 0u) result |= (1u << b);
    }
    g_world_light_occupancy[word] = result;
}

#endif
