cmake_minimum_required(VERSION 3.21)

project(remc1 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REMC1_EXPERIMENTAL_64BIT "Allow building for 64-bit targets (game code is not 64-bit clean yet)" OFF)
option(REMC1_ASAN "Build with AddressSanitizer" OFF)
option(REMC1_BUILD_TESTS "Build the remc1-tests GoogleTest target" ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT REMC1_EXPERIMENTAL_64BIT)
    message(FATAL_ERROR
        "REMC1 currently only works as a 32-bit build (the decompiled code stores pointers in 32-bit ints).\n"
        "On Windows use the 'win-x86' preset (-A Win32). Pass -DREMC1_EXPERIMENTAL_64BIT=ON to override.")
endif()

find_package(SDL2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Compiler settings shared by every target
# ---------------------------------------------------------------------------
add_library(remc1_options INTERFACE)
target_include_directories(remc1_options INTERFACE ${CMAKE_SOURCE_DIR})
target_compile_definitions(remc1_options INTERFACE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
if(REMC1_EXPERIMENTAL_64BIT)
    target_compile_definitions(remc1_options INTERFACE REMC1_EXPERIMENTAL_64BIT)
endif()
if(MSVC)
    # /bigobj: sub_main.cpp is ~110k lines. /wd4996: POSIX/CRT names. /permissive-: off, decompiled code relies on MSVC leniency.
    target_compile_options(remc1_options INTERFACE /utf-8 /bigobj /Zc:__cplusplus /wd4996 /MP)
    if(REMC1_ASAN)
        # MSVC ASan is incompatible with /RTC1 and incremental linking; the runtime DLL is copied
        # next to the executables below.
        foreach(flags CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
            string(REPLACE "/RTC1" "" ${flags} "${${flags}}")
        endforeach()
        target_compile_options(remc1_options INTERFACE /fsanitize=address /Zi)
        target_link_options(remc1_options INTERFACE /INCREMENTAL:NO /DEBUG)
        get_filename_component(REMC1_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(REMC1_ASAN_DLL "${REMC1_MSVC_BIN}/clang_rt.asan_dynamic-i386.dll")
        else()
            set(REMC1_ASAN_DLL "${REMC1_MSVC_BIN}/clang_rt.asan_dynamic-x86_64.dll")
        endif()
    endif()
else()
    target_compile_options(remc1_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive> -Wno-narrowing -fno-strict-aliasing -fwrapv)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        target_compile_options(remc1_options INTERFACE -fsigned-char)
    endif()
    if(REMC1_ASAN)
        target_compile_options(remc1_options INTERFACE -fsanitize=address,undefined)
        target_link_options(remc1_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# Engine: all game + platform code, as a static library so tests can link it
# ---------------------------------------------------------------------------
set(REMC1_ENGINE_SOURCES
    sub_main.cpp
    engine/Basic.cpp
    engine/CommandLineParser.cpp
    portability/fcaseopen.cpp
    portability/port_filesystem.cpp
    portability/port_outputs.cpp
    portability/port_sdl_vga_mouse.cpp
    portability/port_time.cpp
    portability/Settings.cpp
    portability/InputScript.cpp
    portability/port_timer.cpp
    portability/port_watchdog.cpp
    portability/SdlPresenter.cpp
    utilities/Compare.cpp
    utilities/DataFileIO.cpp
)
if(WIN32)
    list(APPEND REMC1_ENGINE_SOURCES portability/dirent.c)
endif()

add_library(remc1_engine STATIC ${REMC1_ENGINE_SOURCES})
target_link_libraries(remc1_engine PUBLIC remc1_options
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    nlohmann_json::nlohmann_json)
target_compile_definitions(remc1_engine PUBLIC SDL_MAIN_HANDLED)

# ---------------------------------------------------------------------------
# Game executable
# ---------------------------------------------------------------------------
add_executable(remc1 REMC1.cpp)
target_link_libraries(remc1 PRIVATE remc1_engine)
if(MSVC)
    set_property(TARGET remc1 PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:remc1>")
endif()

# Runtime assets next to the executable
add_custom_command(TARGET remc1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:remc1>
    VERBATIM)
if(REMC1_ASAN_DLL)
    add_custom_command(TARGET remc1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REMC1_ASAN_DLL}" $<TARGET_FILE_DIR:remc1>
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(REMC1_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
