# This file is part of Agentgram, a fork of Telegram Desktop.
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

# Agentgram keeps its few changes to the desktop-app submodules as
# patches and applies them to the checked out submodules at configure
# time, so CI, the Windows build script and local builds all get them.
find_package(Git QUIET)

function(agentgram_apply_patch submodule patch)
    if (NOT GIT_FOUND)
        message(WARNING "Agentgram: git not found, ${patch} is not applied.")
        return()
    endif()
    set(directory ${CMAKE_CURRENT_SOURCE_DIR}/${submodule})
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${patch}
        WORKING_DIRECTORY ${directory}
        RESULT_VARIABLE already
        OUTPUT_QUIET
        ERROR_QUIET)
    if (already EQUAL 0)
        return()
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${patch}
        WORKING_DIRECTORY ${directory}
        RESULT_VARIABLE result)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "Agentgram: could not apply ${patch}.")
    endif()
    message(STATUS "Agentgram: applied ${patch}")
endfunction()

agentgram_apply_patch(lib_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/build/agentgram/patches/lib_ui-agentui.patch)
