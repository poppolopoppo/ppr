module game.tests;

import engine.core;

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // game tests
    // ------------------------------------------------------------------

    const UnitTest &gameConfigTests() noexcept;
    const UnitTest &gameDisplayTests() noexcept;
    const UnitTest &gameContentTests() noexcept;
    const UnitTest &gameWorldJobTests() noexcept;
    const UnitTest &gameMovementTests() noexcept;
    const UnitTest &gameMetricTests() noexcept;
    const UnitTest &gameBiomeContentTests() noexcept;
    const UnitTest &gameFeatureContentTests() noexcept;
    const UnitTest &gameWorldContentTests() noexcept;
    const UnitTest &gamePackContentTests() noexcept;
    const UnitTest &gameContentDescriptorTests() noexcept;
    const UnitTest &gameWorldAssemblyTests() noexcept;
    const UnitTest &gameCheckpointTests() noexcept;
    const UnitTest &gameLegacyFixtureTests() noexcept;
    const UnitTest &gameWorldgenPreviewTests() noexcept;

    // Defined here rather than as an inline constexpr umbrella in
    // Game.Tests.cppm: same MSVC C1001 shape as engine.tests.core/app.
    extern const UnitTest game = UnitTest::Named("game") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            gameConfigTests(),
            gameDisplayTests(),
            gameContentTests(),
            gameWorldJobTests(),
            gameMovementTests(),
            gameMetricTests(),
            gameBiomeContentTests(),
            gameFeatureContentTests(),
            gameWorldContentTests(),
            gamePackContentTests(),
            gameContentDescriptorTests(),
            gameWorldAssemblyTests(),
            gameCheckpointTests(),
            gameLegacyFixtureTests(),
            gameWorldgenPreviewTests(),
        });
    };
} // namespace pP::game::tests
