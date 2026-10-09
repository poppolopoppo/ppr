# Engine tests are organized as one executable per library (exolithic layout):
#   core/  -> engine.tests.core  (GLFW-free, depends only on engine.core)
#   sim/   -> engine.tests.sim   (GLFW-free, chunk grid / tick / snapshot)
#   app/   -> engine.tests.app   (depends on engine.app, links glfw privately)
#   asset/ -> engine.tests.asset (CPU image decode + mesh convert + GPU caches;
#            image group owned by Lane A, mesh/GPU groups by later lanes)
# Each subdirectory defines its own runner + CTest entry; this file only aggregates.

set(PPR_TEST_LOOPS "1" CACHE STRING "Loop count for CTest and aggregate engine test runs")
if(NOT PPR_TEST_LOOPS MATCHES "^[0-9]+$" OR NOT PPR_TEST_LOOPS MATCHES "[1-9]")
    message(FATAL_ERROR "PPR_TEST_LOOPS must be a positive decimal integer")
endif()

add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(sim)
add_subdirectory(app)
add_subdirectory(asset)

add_custom_target(run-engine-tests
    DEPENDS engine.tests.core engine.tests.sim engine.tests.app engine.tests.asset
    COMMAND engine.tests.core --loop ${PPR_TEST_LOOPS}
    COMMAND engine.tests.sim --loop ${PPR_TEST_LOOPS}
    COMMAND engine.tests.app --loop ${PPR_TEST_LOOPS}
    COMMAND engine.tests.asset --loop ${PPR_TEST_LOOPS}
)
