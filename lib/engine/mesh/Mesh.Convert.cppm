module;
#include "pP/Macros.h"
export module engine.mesh:convert;

import engine.core;
import engine.math;
import :types;

import std;

// Synchronous thread-safe static glTF/GLB conversion
// (docs/plans/asset-pipeline.md §§2.3/3). One Mango import plus one per-job
// UniqueBuffer per call; no shared mutable state, so concurrent calls on
// distinct files are safe. Mango decode-server contention, if any, stays
// inside Mango. Async/worker dispatch lives one level up, never here.

export namespace pP::mesh {
    // Imports a STATIC glTF/GLB scene and converts it to engine assets.
    // dir is the asset root; file is relative to dir (may include subfolders).
    // Only .gltf/.glb are accepted (OBJ/FBX are deferred entirely).
    // Errors: silent-empty Scene → invalid_argument; missing POSITION or bad
    // indices → invalid_argument; joint/skin data → function_not_supported;
    // AlphaMode::blend and authored clearcoat/sheen/anisotropy/opacity content
    // → function_not_supported; Mango failures → import_failed.
    // Over-limit inputs (MeshLimits) → invalid_argument, fail-closed.
    [[nodiscard]] Expected<SceneAsset> importAndConvert(
        const std::filesystem::path &dir, std::string_view file, const MeshLimits &limits = kDefaultMeshLimits);
}
