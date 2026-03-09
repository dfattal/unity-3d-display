// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Standalone OpenXR session for editor preview.
// Loads the DisplayXR runtime directly (bypassing Unity's OpenXR loader)
// and manages its own instance/session lifecycle.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define DISPLAYXR_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define DISPLAYXR_EXPORT __attribute__((visibility("default")))
#else
#define DISPLAYXR_EXPORT
#endif

/// Start a standalone OpenXR session for editor preview.
/// Loads the runtime from the JSON manifest, creates instance/session,
/// and begins compositing into a shared IOSurface.
/// @param runtime_json_path Path to the OpenXR runtime JSON manifest.
/// @return 1 on success, 0 on failure.
DISPLAYXR_EXPORT int displayxr_standalone_start(const char *runtime_json_path);

/// Stop the standalone session and release all resources.
/// Safe to call even if not running.
DISPLAYXR_EXPORT void displayxr_standalone_stop(void);

/// Whether the standalone session is currently running.
DISPLAYXR_EXPORT int displayxr_standalone_is_running(void);

/// Poll events and update eye tracking. Call from EditorApplication.update.
/// Also drives frame submission to keep the session alive.
DISPLAYXR_EXPORT void displayxr_standalone_poll(void);

/// Get display info from the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_get_display_info(
    float *display_width_m, float *display_height_m,
    uint32_t *pixel_width, uint32_t *pixel_height,
    float *nominal_x, float *nominal_y, float *nominal_z,
    float *scale_x, float *scale_y,
    int *supports_mode_switch, int *is_valid);

/// Get eye positions from the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_get_eye_positions(
    float *lx, float *ly, float *lz,
    float *rx, float *ry, float *rz,
    int *is_tracked);

/// Get the shared texture info from the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_get_shared_texture(
    void **native_ptr, uint32_t *width, uint32_t *height, int *ready);

#ifdef __cplusplus
}
#endif
