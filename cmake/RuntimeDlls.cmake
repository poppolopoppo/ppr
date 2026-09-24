# Runtime DLLs must sit beside the executable so the Windows loader cannot resolve a different one from PATH; this prevented test executables from silently loading a foreign slang-compiler.dll from C:\VulkanSDK.
function(ppr_stage_runtime_dlls target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND_EXPAND_LISTS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
    )
  endif()
endfunction()
