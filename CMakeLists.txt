cmake_minimum_required(VERSION 3.20)
project(keychron_battery VERSION 1.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "keychron_battery targets Windows (Win32/HID APIs)")
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000000
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)
set_source_files_properties(
    assets/tray.rc
    assets/cli.rc
    PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    set(KC_WARN_FLAGS /W4 /WX /utf-8)
    set(KC_OPT_FLAGS $<$<CONFIG:Release>:/O2>)
    set(KC_LINK_OPT_FLAGS "")
else()
    set(KC_WARN_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wundef -Werror)
    set(KC_OPT_FLAGS
        $<$<CONFIG:Release>:-Os>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
    set(KC_LINK_OPT_FLAGS
        $<$<CONFIG:Release>:-Wl,--gc-sections>
        $<$<CONFIG:Release>:-s>
    )
endif()

add_library(keychron_pure STATIC
    src/protocol.c
    src/glyph_blit.c
)
target_include_directories(keychron_pure PUBLIC src)
target_compile_options(keychron_pure PRIVATE ${KC_WARN_FLAGS} ${KC_OPT_FLAGS})

add_library(keychron_win STATIC
    src/hid_win.c
    src/icon_render.c
    src/settings_win.c
)
target_include_directories(keychron_win PUBLIC src)
target_compile_options(keychron_win PRIVATE ${KC_WARN_FLAGS} ${KC_OPT_FLAGS})
target_link_libraries(keychron_win PUBLIC keychron_pure hid cfgmgr32 gdi32 user32 advapi32)

add_executable(keychron-battery-tray WIN32
    src/tray_main.c
    assets/tray.rc
)
target_compile_options(keychron-battery-tray PRIVATE ${KC_WARN_FLAGS} ${KC_OPT_FLAGS})
if(MSVC)
    target_link_options(keychron-battery-tray PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)
else()
    target_link_options(keychron-battery-tray PRIVATE -municode -mwindows ${KC_LINK_OPT_FLAGS})
endif()
target_link_libraries(keychron-battery-tray PRIVATE keychron_win shell32 user32 gdi32 advapi32 hid)

add_executable(keychron-battery
    src/cli_main.c
    assets/cli.rc
)
target_compile_options(keychron-battery PRIVATE ${KC_WARN_FLAGS} ${KC_OPT_FLAGS})
if(MSVC)
    target_link_options(keychron-battery PRIVATE /ENTRY:wmainCRTStartup)
else()
    target_link_options(keychron-battery PRIVATE -municode ${KC_LINK_OPT_FLAGS})
endif()
target_link_libraries(keychron-battery PRIVATE keychron_win hid)

install(TARGETS keychron-battery-tray keychron-battery RUNTIME DESTINATION bin)

enable_testing()
add_executable(keychron_tests
    tests/test_core.c
)
target_compile_options(keychron_tests PRIVATE ${KC_WARN_FLAGS})
target_link_libraries(keychron_tests PRIVATE keychron_pure)
add_test(NAME core_tests COMMAND keychron_tests)

add_executable(keychron_win_tests
    tests/test_win.c
)
target_compile_options(keychron_win_tests PRIVATE ${KC_WARN_FLAGS})
target_link_libraries(keychron_win_tests PRIVATE keychron_win)
add_test(NAME win_icon_tests COMMAND keychron_win_tests)
