# Assets are build inputs in their own right: an up-to-date executable must
# still receive edits and recover missing deployment files without relinking.
function(ppr_stage_assets target)
    set(stage_commands)
    foreach(directory IN LISTS ARGN)
        list(APPEND stage_commands
            COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/assets/${directory}"
                "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
                "-DDEPLOY_ROOT=$<TARGET_FILE_DIR:${target}>/${directory}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageAssets.cmake"
        )
    endforeach()
    add_custom_target(${target}.assets
        ${stage_commands}
        COMMENT "Staging assets for ${target}"
        VERBATIM
    )
    add_dependencies(${target} ${target}.assets)
endfunction()
