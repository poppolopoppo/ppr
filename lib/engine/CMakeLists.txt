add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(shader)
add_subdirectory(rhi)
add_subdirectory(app)
# Dev-only test aggregates (engine.tests, engine.tests.core, engine.tests.app):
# excluded from shipping configs with tests disabled (e.g. msvc-rel).
# NOTE: tolerates undefined BUILD_TESTING as ON (CTest convention; root
# CMakeLists calls enable_testing() without include(CTest), so the option only
# exists when a preset sets it). msvc-rel sets it OFF explicitly.
if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()
