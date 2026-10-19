# Define the executable target
add_executable(app.game main.cpp)

target_sources(app.game
    PUBLIC
        FILE_SET CXX_MODULES FILES
            ui/Game.UI.cppm
            ui/Game.UI.Model.cppm
            Game.Application.cppm
            Game.WorldgenWorkshop.cppm
            colony/Agents.cppm
            ui/AssetBrowserPanel.cppm
            colony/Buildings.cppm
            colony/Checkpoint.cppm
            colony/Colony.cppm
            colony/ColonyDriver.cppm
            ui/ColonyPanel.cppm
            colony/ColonySmoke.cppm
            colony/ColonyTranslator.cppm
            colony/Content.cppm
            colony/Content.Authoring.cppm
            colony/Metric.cppm
            colony/DigTool.cppm
            colony/Pathfinding.cppm
            colony/SimFramePacket.cppm
            colony/WorldGen.cppm
            colony/WorldGen.Preview.cppm
            ui/WorldGenPanel.cppm
            colony/WorldProgress.cppm
            colony/WorldJob.cppm
            Game.Config.cppm
            Game.Display.cppm
    PRIVATE
        ui/Game.UI.cpp
        ui/Game.UI.Model.cpp
        Game.Application.cpp
        Game.WorldgenWorkshop.cpp
        colony/Agents.cpp
        ui/AssetBrowserPanel.cpp
        colony/Buildings.cpp
        colony/Checkpoint.cpp
        colony/Checkpoint.Codec.cpp
        colony/Checkpoint.Validation.cpp
        colony/Colony.cpp
        colony/ColonyDriver.cpp
        colony/ColonyDriver.Checkpoint.cpp
        colony/ColonyDriver.Edits.cpp
        ui/ColonyPanel.cpp
        colony/ColonySmoke.cpp
        colony/WorldGen.Capture.cpp
        colony/ColonyTranslator.cpp
        colony/Content.cpp
        colony/Content.Pack.cpp
        colony/Content.Descriptors.cpp
        colony/Content.Assembly.cpp
        colony/Content.Source.cpp
        colony/Content.Presentation.cpp
        colony/Content.Authoring.cpp
        colony/Metric.cpp
        colony/DigTool.cpp
        colony/Pathfinding.cpp
        colony/SimFramePacket.cpp
        colony/SimFramePacket.Diagnostics.cpp
        colony/WorldGen.cpp
        colony/WorldGen.Fixture.cpp
        colony/WorldGen.Plan.cpp
        colony/WorldGen.Export.cpp
        colony/WorldGen.Preview.cpp
        colony/WorldGen.Preview.Export.cpp
        ui/WorldGenPanel.cpp
        colony/WorldJob.cpp
        Game.Config.cpp
        Game.Display.cpp
    )

setup_ppr_project(app.game
  INTERNAL_PUBLIC_DEPS engine.core engine.asset engine.app engine.math engine.shader engine.rhi engine.sim engine.image
)

# Slice 4: driver-owned physics Scene + chunk colliders (explicit grant).
# PRIVATE shape mirrors engine.tests.sim; box2d stays behind engine.physics.
target_link_libraries(app.game PRIVATE engine.physics)

# Always run on an explicit game build: data-only edits must reach deployment
# without requiring a C++ relink, including removal of deleted source assets.
add_custom_target(worldgen.assets
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/assets/worldgen"
        "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
        "-DDEPLOY_ROOT=$<TARGET_FILE_DIR:app.game>/worldgen"
        -P "${CMAKE_SOURCE_DIR}/cmake/StageWorldgen.cmake"
    COMMENT "Staging world-generation content"
)
add_dependencies(app.game worldgen.assets)

# The UI service always reads its pinned font set at startup, so `fonts` is
# a hard runtime requirement of app.game (not an optional content tree:
# a missing directory must fail staging loudly, never degrade silently).
set(game_asset_directories shaders textures fonts)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/meshes")
    list(APPEND game_asset_directories meshes)
endif()
ppr_stage_assets(app.game ${game_asset_directories})

add_custom_target(run-app-game
    DEPENDS app.game
    COMMAND $<TARGET_FILE:app.game>
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:app.game>"
    COMMENT "Running app.game"
    USES_TERMINAL
)

# Game-owned invocation compatibility tests (Phase 2 gate): pure
# parse/resolve coverage for selectors/conflicts, legacy value forms,
# profiling triggers, startup booleans, configuration precedence, and help
# without configuration-file availability. Lives in the game build area so
# engine test tiers never depend upward on game modules.
if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_executable(game.tests Game.Tests.Main.cpp)

    target_sources(game.tests
        PUBLIC
            FILE_SET CXX_MODULES FILES
                ui/Game.UI.cppm
                ui/Game.UI.Model.cppm
                ui/ColonyPanel.cppm
                Game.Application.cppm
                Game.WorldgenWorkshop.cppm
                Game.Config.cppm
                Game.Display.cppm
                Game.Tests.cppm
                colony/Content.cppm
                colony/Content.Authoring.cppm
                colony/Metric.cppm
                colony/Agents.cppm
                ui/AssetBrowserPanel.cppm
                colony/Buildings.cppm
                colony/Checkpoint.cppm
                colony/Colony.cppm
                colony/ColonyDriver.cppm
                colony/ColonyTranslator.cppm
                colony/DigTool.cppm
                colony/Pathfinding.cppm
                colony/WorldGen.cppm
                colony/WorldGen.Preview.cppm
                ui/WorldGenPanel.cppm
                colony/SimFramePacket.cppm
                colony/WorldProgress.cppm
                colony/WorldJob.cppm
        PRIVATE
            ui/Game.UI.cpp
            ui/Game.UI.Model.cpp
            ui/Game.UI.Tests.cpp
            ui/ColonyPanel.cpp
            Game.Config.cpp
            Game.Display.cpp
            Game.WorldgenWorkshop.cpp
            Game.Tests.cpp
            Game.Config.Tests.cpp
            Game.Display.Tests.cpp
            colony/Content.cpp
            colony/Content.Pack.cpp
            colony/Content.Descriptors.cpp
            colony/Content.Assembly.cpp
            colony/Content.Source.cpp
            colony/Content.Presentation.cpp
            colony/Content.Authoring.cpp
            ui/AssetBrowserPanel.cpp
            ui/AssetBrowserPanel.Tests.cpp
            colony/Content.Tests.cpp
            colony/Content.Biomes.Tests.cpp
            colony/Content.Features.Tests.cpp
            colony/Content.Worlds.Tests.cpp
            colony/Content.Pack.Tests.cpp
            colony/Content.Source.Tests.cpp
            colony/Content.TestFixtures.cpp
            colony/Content.Descriptors.Tests.cpp
            colony/Checkpoint.Tests.cpp
            colony/Metric.cpp
            colony/Metric.Tests.cpp
            colony/Agents.cpp
            colony/Buildings.cpp
            colony/Checkpoint.cpp
            colony/Checkpoint.Codec.cpp
            colony/Checkpoint.Validation.cpp
            colony/Colony.cpp
            colony/ColonyDriver.cpp
            colony/ColonyDriver.Checkpoint.cpp
            colony/ColonyDriver.Edits.cpp
            colony/ColonyTranslator.cpp
            colony/DigTool.cpp
            colony/Pathfinding.cpp
            colony/WorldGen.cpp
            colony/WorldGen.Fixture.cpp
            colony/WorldGen.Fixture.Tests.cpp
            colony/WorldGen.Plan.cpp
            colony/WorldGen.Export.cpp
            colony/WorldGen.Preview.cpp
            colony/WorldGen.Preview.Export.cpp
            colony/WorldGen.Preview.Tests.cpp
            ui/WorldGenPanel.cpp
            colony/SimFramePacket.cpp
            colony/SimFramePacket.Diagnostics.cpp
            colony/WorldJob.cpp
            colony/WorldJob.Tests.cpp
            colony/Movement.Tests.cpp
        )

    setup_ppr_project(game.tests
      INTERNAL_PUBLIC_DEPS engine.core engine.asset engine.app engine.math engine.sim engine.image engine.tests
    )
    target_link_libraries(game.tests PRIVATE engine.physics)
    add_dependencies(game.tests worldgen.assets)

    target_include_directories(game.tests PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

    # MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
    if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
        target_link_options(game.tests PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
    endif()

    # Shipped-content and checkpoint coverage measured 358 s for one complete
    # loop. The two worldgen preview replay/crop leaves dominate that budget
    # and own a separate entry; the remaining preview widget leaf stays here.
    ppr_add_test_entry(TARGET game.tests
        EXCLUDE "game/worldgen_preview/bounded_samples_replay_and_export_the_exact_visible_draft,game/worldgen_preview/whole_world_crops_preserve_production_formations_seams_and_matter"
        LOOPS
        TIMEOUT 600
        LABELS unit game cpu
    )

    # Heavy worldgen preview leaves: exact-visible-draft sample replay/export
    # and whole-world crop formation checks. PROCESSORS 4 reserves four CTest
    # scheduling slots for this entry.
    ppr_add_test_entry(TARGET game.tests
        NAME worldgen_preview
        FILTER "game/worldgen_preview/bounded_samples_replay_and_export_the_exact_visible_draft,game/worldgen_preview/whole_world_crops_preserve_production_formations_seams_and_matter"
        LOOPS
        PROCESSORS 4
        TIMEOUT 600
        LABELS unit game worldgen
    )

    ppr_add_test_entry(TARGET app.game
        NAME worldgen_frontier
        ARGS "--validate-worldgen;--worldgen-profile=frontier"
        TIMEOUT 300
        LABELS game worldgen cpu
    )

    ppr_add_test_entry(TARGET app.game
        NAME worldgen_hollows
        ARGS "--validate-worldgen;--worldgen-profile=hollows"
        TIMEOUT 300
        LABELS game worldgen cpu
    )
endif()
