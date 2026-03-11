# Copyright 2024-2026, DisplayXR contributors
# SPDX-License-Identifier: BSL-1.0
#
# Build the displayxr_unity native plugin for OpenXR function interception.
# Produces: displayxr_unity.dll (Windows) or displayxr_unity.bundle (macOS)

cmake_minimum_required(VERSION 3.16)
project(displayxr_unity LANGUAGES CXX C)

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
    displayxr_hooks.cpp
    displayxr_kooima.cpp
    displayxr_shared_state.cpp
    displayxr_readback.cpp
    displayxr_standalone.cpp
    display3d_view.c
    camera3d_view.c
)

set(HEADERS
    displayxr_hooks.h
    displayxr_kooima.h
    displayxr_shared_state.h
    displayxr_readback.h
    displayxr_extensions.h
    displayxr_standalone.h
    display3d_view.h
    camera3d_view.h
)

# Apple-only: IOSurface helpers (Objective-C)
if(APPLE)
    list(APPEND SOURCES displayxr_metal.m displayxr_standalone_metal.m)
    list(APPEND HEADERS displayxr_metal.h displayxr_standalone_metal.h)
endif()

# --- Build shared library ---
# Use MODULE on macOS so BUNDLE property works (creates .bundle directory)
if(APPLE)
    add_library(displayxr_unity MODULE ${SOURCES} ${HEADERS})
else()
    add_library(displayxr_unity SHARED ${SOURCES} ${HEADERS})
endif()

target_include_directories(displayxr_unity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENXR_INCLUDE_DIR}
)

# Symbol visibility
if(MSVC)
    target_compile_definitions(displayxr_unity PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(displayxr_unity PRIVATE -fvisibility=hidden)
endif()

# --- Apple framework linking ---
if(APPLE)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(displayxr_unity PRIVATE
        ${IOSURFACE_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
    )
    # Enable ARC for Objective-C files
    set_source_files_properties(displayxr_metal.m displayxr_standalone_metal.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# --- Install to Unity plugin directories ---
if(WIN32)
    set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/Windows/x64")
    # Set per-config output dirs to prevent MSVC multi-config subdirectories
    set_target_properties(displayxr_unity PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUTPUT_DIR}"
    )
elseif(APPLE)
    set(PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Plugins/macOS")
    set_target_properties(displayxr_unity PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.displayxr.unity.native"
    )
endif()

# --- Summary ---
message(STATUS "displayxr_unity: OpenXR headers from ${OPENXR_INCLUDE_DIR}")
message(STATUS "displayxr_unity: Output to ${PLUGIN_OUTPUT_DIR}")
