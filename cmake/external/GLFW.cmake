# GLFW - Window and input management library
# Provides window creation and input handling for the engine's display system

find_package(glfw3 REQUIRED CONFIG)

# Mark GLFW includes as SYSTEM to suppress warnings from external headers
get_target_property(glfw_inc glfw INTERFACE_INCLUDE_DIRECTORIES)
if(glfw_inc)
    set_target_properties(glfw PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${glfw_inc}")
endif()

set_target_properties(glfw PROPERTIES CXX_MODULE_STD OFF)
