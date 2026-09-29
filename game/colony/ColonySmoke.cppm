export module game.colony.smoke;

import std;

export namespace pP::colony {
    /// Run one headless, offscreen colony frame and verify a generated cell by GPU readback.
    [[nodiscard]] std::error_code runColonySmoke(std::span<const char *const> argv);
}
