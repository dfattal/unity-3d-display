# Copyright 2024-2026, Monado 3D Display contributors
# SPDX-License-Identifier: BSL-1.0
#
# Build the monado3d_unity native plugin for OpenXR function interception.
# Produces: monado3d_unity.dll (Windows) or monado3d_unity.bundle (macOS)

cmake_minimum_required(VERSION 3.16)
project(monado3d_unity LANGUAGES CXX C)

if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- OpenXR SDK headers ---
# Try to find the OpenXR SDK. If not found, download headers.
find_package(OpenXR QUIET)

if(NOT OpenXR_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.0.34
        GIT_SHALLOW TRUE
    )
    # Only fetch headers — don't build the full SDK (avoids jsoncpp/loader deps)
    FetchContent_GetProperties(openxr_sdk)
    if(NOT openxr_sdk_POPULATED)
        FetchContent_Populate(openxr_sdk)
    endif()
    set(OPENXR_INCLUDE_DIR "${openxr_sdk_SOURCE_DIR}/include")
else()
    get_target_property(OPENXR_INCLUDE_DIR OpenXR::Headers INTERFACE_INCLUDE_DIRECTORIES)
endif()

# --- Plugin sources ---
set(SOURCES
    monado3d_hooks.cpp
    monado3d_kooima.cpp
    monado3d_shared_state.cpp
    monado3d_readback.cpp
    display3d_view.c
)

set(HEADERS
    monado3d_hooks.h
    monado3d_kooima.h
    monado3d_shared_state.h
    monado3d_readback.h
    monado3d_extensions.h
    display3d_view.h
)

# Apple-only: IOSurface helper (Objective-C)
if(APPLE)
    list(APPEND SOURCES monado3d_metal.m)
    list(APPEND HEADERS monado3d_metal.h)
endif()

# --- Build shared library ---
# Use MODULE on macOS so BUNDLE property works (creates .bundle directory)
if(APPLE)
    add_library(monado3d_unity MODULE ${SOURCES} ${HEADERS})
else()
    add_library(monado3d_unity SHARED ${SOURCES} ${HEADERS})
endif()

target_include_directories(monado3d_unity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENXR_INCLUDE_DIR}
)

# Symbol visibility
if(MSVC)
    target_compile_definitions(monado3d_unity PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(monado3d_unity PRIVATE -fvisibility=hidden)
endif()

# --- Apple framework linking ---
if(APPLE)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    target_link_libraries(monado3d_unity PRIVATE
        ${IOSURFACE_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${METAL_FRAMEWORK}
    )
    # Enable ARC for Objective-C files
    set_source_files_properties(monado3d_metal.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# --- Install to Unity plugin directories ---
if(WIN32)
    set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/Windows/x64")
    # Set per-config output dirs to prevent MSVC multi-config subdirectories
    set_target_properties(monado3d_unity PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
    )
elseif(APPLE)
    set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/macOS")
    set_target_properties(monado3d_unity PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.monado.display3d.native"
    )
endif()

# --- Summary ---
message(STATUS "monado3d_unity: OpenXR headers from ${OPENXR_INCLUDE_DIR}")
message(STATUS "monado3d_unity: Output to ${PLUGIN_OUTPUT_DIR}")
