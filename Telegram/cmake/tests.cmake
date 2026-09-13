# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_text WIN32)
init_target(test_text "(tests)")

target_include_directories(test_text PRIVATE ${src_loc})

nice_target_sources(test_text ${src_loc}
PRIVATE
    tests/test_main.cpp
    tests/test_main.h
    tests/test_text.cpp
)

nice_target_sources(test_text ${res_loc}
PRIVATE
    qrc/emoji_1.qrc
    qrc/emoji_2.qrc
    qrc/emoji_3.qrc
    qrc/emoji_4.qrc
    qrc/emoji_5.qrc
    qrc/emoji_6.qrc
    qrc/emoji_7.qrc
    qrc/emoji_8.qrc
)

target_link_libraries(test_text
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::external_qt
    desktop-app::external_qt_static_plugins
)

set_target_properties(test_text PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_text)

target_prepare_qrc(test_text)

if (APPLE)
    add_custom_command(TARGET test_text POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:test_text>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/test_text.rcc"
            "${CMAKE_BINARY_DIR}/lib_ui.rcc"
            "$<TARGET_FILE_DIR:test_text>/Contents/Resources/"
    )
endif()

add_executable(test_mp4_header)
init_target(test_mp4_header "(tests)")

target_include_directories(test_mp4_header PRIVATE ${src_loc})

nice_target_sources(test_mp4_header ${src_loc}
PRIVATE
    media/streaming/media_streaming_mp4_header.h
    test/test_mp4_header.cpp
)

set_target_properties(test_mp4_header PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_mp4_header)

add_executable(test_mp4_index)
init_target(test_mp4_index "(tests)")

target_include_directories(test_mp4_index PRIVATE ${src_loc})

nice_target_sources(test_mp4_index ${src_loc}
PRIVATE
    media/streaming/media_streaming_mp4_header.h
    media/streaming/media_streaming_mp4_fragment.h
    media/streaming/media_streaming_mp4_index.h
    test/test_mp4_index.cpp
)

set_target_properties(test_mp4_index PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_mp4_index)

add_executable(test_streaming_read_stall)
init_target(test_streaming_read_stall "(tests)")

target_include_directories(test_streaming_read_stall PRIVATE ${src_loc})

nice_target_sources(test_streaming_read_stall ${src_loc}
PRIVATE
    media/streaming/media_streaming_read_stall.h
    test/test_streaming_read_stall.cpp
)

set_target_properties(test_streaming_read_stall PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_streaming_read_stall)

add_executable(test_streaming_seek)
init_target(test_streaming_seek "(tests)")

target_include_directories(test_streaming_seek PRIVATE ${src_loc})

nice_target_sources(test_streaming_seek ${src_loc}
PRIVATE
    media/streaming/media_streaming_mp4_seek.h
    test/test_streaming_seek.cpp
)

set_target_properties(test_streaming_seek PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_streaming_seek)

add_executable(test_streaming_startup)
init_target(test_streaming_startup "(tests)")

target_include_directories(test_streaming_startup PRIVATE ${src_loc})

nice_target_sources(test_streaming_startup ${src_loc}
PRIVATE
    media/streaming/media_streaming_startup.h
    test/test_streaming_startup.cpp
)

set_target_properties(test_streaming_startup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_streaming_startup)

add_executable(test_streaming_cache)
init_target(test_streaming_cache "(tests)")

target_include_directories(test_streaming_cache PRIVATE ${src_loc})

nice_target_sources(test_streaming_cache ${src_loc}
PRIVATE
    media/streaming/media_streaming_cache.h
    test/test_streaming_cache.cpp
)

set_target_properties(test_streaming_cache PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_streaming_cache)

add_executable(test_streaming_mpv)
init_target(test_streaming_mpv "(tests)")

target_include_directories(test_streaming_mpv PRIVATE ${src_loc})

nice_target_sources(test_streaming_mpv ${src_loc}
PRIVATE
    media/streaming/media_streaming_mpv_http.h
    test/test_streaming_mpv.cpp
)

set_target_properties(test_streaming_mpv PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_streaming_mpv)
