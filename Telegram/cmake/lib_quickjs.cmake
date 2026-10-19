# This file is part of Agentgram, a fork of Telegram Desktop.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

# quickjs-ng: the JavaScript engine that runs Bot Studio bots in-process.
# Only the engine itself is built, without quickjs-libc, so scripts have no
# file or process access; everything they can do is given by bot_studio.

add_library(lib_quickjs STATIC)
init_target(lib_quickjs "(external)")

add_library(desktop-app::lib_quickjs ALIAS lib_quickjs)

set(quickjs_loc ${third_party_loc}/quickjs)

nice_target_sources(lib_quickjs ${quickjs_loc}
PRIVATE
    dtoa.c
    dtoa.h
    libregexp.c
    libregexp.h
    libunicode.c
    libunicode.h
    quickjs.c
    quickjs.h
)

target_include_directories(lib_quickjs
PUBLIC
    ${quickjs_loc}
)

target_compile_definitions(lib_quickjs
PRIVATE
    _GNU_SOURCE
    QUICKJS_NG_BUILD
)

target_compile_options(lib_quickjs PRIVATE -w -funsigned-char)
target_link_libraries(lib_quickjs PUBLIC m)
