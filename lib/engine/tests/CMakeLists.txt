# Engine tests are organized as one executable per library (exolithic layout):
#   core/  -> EngineCoreTests  (GLFW-free, depends only on EngineCore)
#   app/   -> EngineAppTests   (depends on EngineApp, links glfw privately)
# Each subdirectory defines its own runner + CTest entry; this file only aggregates.

add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(app)

add_custom_target(run-engine-tests
    DEPENDS EngineCoreTests EngineAppTests
    COMMAND EngineCoreTests
    COMMAND EngineAppTests
)
