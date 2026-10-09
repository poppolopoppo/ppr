module game.tests;

import engine.core;

namespace pP::game::tests {
    const UnitTest &gameConfigTests() noexcept;
    const UnitTest &gameDisplayTests() noexcept;

    // Defined here rather than as an inline constexpr umbrella in
    // Game.Tests.cppm: same MSVC C1001 shape as engine.tests.core/app.
    extern const UnitTest game = UnitTest::Named("game") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            gameConfigTests(),
            gameDisplayTests(),
        });
    };
} // namespace pP::game::tests
