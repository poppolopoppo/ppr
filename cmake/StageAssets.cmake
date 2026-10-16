# Only the named, build-owned asset trees may be synchronized. Keep unchanged
# files intact while removing stale files and restoring edited/missing assets.
file(REAL_PATH "${BUILD_ROOT}" build_root)
cmake_path(ABSOLUTE_PATH DEPLOY_ROOT NORMALIZE OUTPUT_VARIABLE deploy_root)
# REAL_PATH warns for a missing tree. Resolve its nearest existing ancestor to
# check redirects while permitting first-build and deleted-directory recovery.
set(existing_parent "${deploy_root}")
while(NOT EXISTS "${existing_parent}")
    cmake_path(GET existing_parent PARENT_PATH parent)
    if(parent STREQUAL existing_parent)
        message(FATAL_ERROR "Cannot resolve asset destination: ${deploy_root}")
    endif()
    set(existing_parent "${parent}")
endwhile()
file(REAL_PATH "${existing_parent}" resolved_parent)
cmake_path(IS_PREFIX build_root "${resolved_parent}" NORMALIZE parent_within_build)
cmake_path(IS_PREFIX build_root "${deploy_root}" NORMALIZE within_build)
cmake_path(GET deploy_root FILENAME leaf)
if(NOT within_build OR NOT parent_within_build OR NOT leaf MATCHES "^(shaders|textures|meshes|fonts)$" OR deploy_root STREQUAL build_root)
    message(FATAL_ERROR "Refusing asset staging outside its build-owned directory: ${deploy_root}")
endif()
if(NOT IS_DIRECTORY "${SOURCE_ROOT}")
    message(FATAL_ERROR "Missing source asset directory: ${SOURCE_ROOT}")
endif()

file(GLOB_RECURSE deployed_files LIST_DIRECTORIES TRUE RELATIVE "${deploy_root}" "${deploy_root}/*")
# Reject redirected children before copying or pruning through them.
foreach(relative IN LISTS deployed_files)
    file(REAL_PATH "${deploy_root}/${relative}" resolved)
    cmake_path(IS_PREFIX deploy_root "${resolved}" NORMALIZE within_deploy)
    if(IS_SYMLINK "${deploy_root}/${relative}" OR NOT within_deploy)
        message(FATAL_ERROR "Refusing redirected asset destination: ${deploy_root}/${relative}")
    endif()
endforeach()
# Children precede their parents so stale directories and path-type changes
# can be removed without touching any still-required sibling.
list(SORT deployed_files ORDER DESCENDING)
foreach(relative IN LISTS deployed_files)
    if(IS_DIRECTORY "${deploy_root}/${relative}")
        if(NOT IS_DIRECTORY "${SOURCE_ROOT}/${relative}")
            file(REMOVE_RECURSE "${deploy_root}/${relative}")
        endif()
    elseif(NOT EXISTS "${SOURCE_ROOT}/${relative}" OR IS_DIRECTORY "${SOURCE_ROOT}/${relative}")
        file(REMOVE "${deploy_root}/${relative}")
    endif()
endforeach()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${SOURCE_ROOT}" "${deploy_root}"
    COMMAND_ERROR_IS_FATAL ANY
)
