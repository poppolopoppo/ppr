# Engine tests are organized as one executable per library (exolithic layout):
#   core/  -> engine.tests.core  (GLFW-free, depends only on engine.core)
#   app/   -> engine.tests.app   (depends on engine.app, links glfw privately)
# Each subdirectory defines its own runner + CTest entry; this file only aggregates.

add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(app)

add_custom_target(run-engine-tests
    DEPENDS engine.tests.core engine.tests.app
    COMMAND engine.tests.core
    COMMAND engine.tests.app
)
