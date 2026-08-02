cmake_minimum_required(VERSION 3.18)
project(nrzloader CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(WARNING
        "The hook engine only implements AArch64. Configure with the NDK "
        "toolchain file and ANDROID_ABI=arm64-v8a for a device build.")
endif()

set(LOADER_SOURCES
    src/core/entry.cpp
    src/core/loader.cpp
    src/core/bindings.cpp
    src/core/v8_bridge.cpp
    src/core/jni_bridge.cpp
    src/core/js_natives.cpp
    src/core/ui_bridge.cpp
    src/core/screen_fx.cpp
    src/core/overlay.cpp
    src/core/apk_assets.cpp
    src/core/event_bus.cpp
    src/core/symbols.cpp
    src/core/module.cpp
    src/core/paths.cpp
    src/core/mod_manifest.cpp
    src/core/elf_image.cpp
    src/core/log.cpp
    src/hook/inline_hook.cpp
    src/hook/arm64_reloc.cpp
    src/hook/vtable_stubs.S
    src/core/vtable_probe.cpp
)

add_library(nrzloader SHARED ${LOADER_SOURCES})
target_include_directories(nrzloader PUBLIC include PRIVATE src)
target_compile_options(nrzloader PRIVATE -Wall -Wextra -fvisibility=hidden)

if(ANDROID)
    find_library(log_lib log)
    find_library(z_lib z)
    # Screen effects draw through the same graphics stack the game uses; both
    # libraries are part of Android itself, so nothing new ships with the mod.
    find_library(gles_lib GLESv2)
    target_link_libraries(nrzloader ${log_lib} ${z_lib} ${gles_lib} dl)
endif()

# Mods only need the public headers.
add_library(mcbe_mod_api INTERFACE)
target_include_directories(mcbe_mod_api INTERFACE include)

option(MCBE_BUILD_EXAMPLES "Build the example mod" ON)
if(MCBE_BUILD_EXAMPLES)
    add_subdirectory(examples/hello_mod)
endif()

# Host side tests, built for aarch64 and run under an emulator.
option(MCBE_BUILD_TESTS "Build the AArch64 test binaries" OFF)
if(MCBE_BUILD_TESTS)
    enable_testing()

    add_executable(test_arm64 tests/test_arm64.cpp tests/prologues.S
        src/hook/arm64_reloc.cpp src/hook/inline_hook.cpp)
    target_include_directories(test_arm64 PRIVATE src include)

    add_executable(test_symbols tests/test_symbols.cpp
        src/core/elf_image.cpp src/core/module.cpp src/core/symbols.cpp src/core/log.cpp)
    target_include_directories(test_symbols PRIVATE src include)
    target_link_libraries(test_symbols dl)

    add_executable(test_paths tests/test_paths.cpp src/core/paths.cpp)
    target_include_directories(test_paths PRIVATE src include)

    add_executable(test_mods tests/test_mods.cpp src/core/mod_manifest.cpp)
    target_include_directories(test_mods PRIVATE src include)

    add_test(NAME arm64_hooks COMMAND test_arm64)
    add_test(NAME directory_layout COMMAND test_paths)
    add_test(NAME symbol_resolution COMMAND test_symbols)
    add_test(NAME mod_manifests COMMAND test_mods)
endif()

# Release builds ship without symbols: the loader is 3 MB otherwise, most of
# it debug information nobody on a phone needs.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(nrzloader PRIVATE -Wl,-s)
endif()
