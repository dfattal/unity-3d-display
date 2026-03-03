// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Thread-safe shared state between Unity's game thread (C# tunables updates)
// and the render thread (OpenXR hook execution).

#pragma once

#include <openxr/openxr.h>
#include <stdint.h>
#include "monado3d_extensions.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Tunables (set from game thread, read from render thread) ---

// Scene transform applied to raw eye positions before Kooima computation.
// Set from C# (game thread) based on camera/display scene hierarchy.
// Maps raw DISPLAY-space eye positions into the app's desired coordinate frame.
typedef struct Monado3DSceneTransform {
    float position[3];    // Translation offset (meters)
    float orientation[4]; // Rotation quaternion (x, y, z, w)
    float zoom_scale;     // Zoom/distance scale: eyes divided by this before Kooima
    uint8_t enabled;      // Whether to apply this transform
} Monado3DSceneTransform;

typedef struct Monado3DTunables {
    float ipd_factor;           // Scales inter-eye distance
    float parallax_factor;      // Scales eye X/Y offset from center
    float perspective_factor;   // Scales eye Z only
    float scale_factor;         // Virtual display size relative to physical
    float convergence_distance; // Override convergence (0 = use nominal)
    float fov_override;         // Override FOV in radians (0 = compute)
    uint8_t camera_centric;     // Use camera-centric parameters
} Monado3DTunables;

// --- Display info (set from render thread, read from game thread) ---

typedef struct Monado3DDisplayInfo {
    float display_width_meters;
    float display_height_meters;
    uint32_t display_pixel_width;
    uint32_t display_pixel_height;
    float nominal_viewer_x;
    float nominal_viewer_y;
    float nominal_viewer_z;
    float recommended_view_scale_x;
    float recommended_view_scale_y;
    uint8_t supports_display_mode_switch;
    uint8_t is_valid;
} Monado3DDisplayInfo;

// --- Eye positions (set from render thread, read from game thread) ---

typedef struct Monado3DEyePositions {
    XrVector3f left_eye;  // Raw left eye position in DISPLAY space
    XrVector3f right_eye; // Raw right eye position in DISPLAY space
    uint8_t is_tracked;   // Whether eye tracking is active
} Monado3DEyePositions;

// --- Window-space layer descriptor (set from game thread, read from render thread) ---

#define MONADO3D_MAX_WINDOW_LAYERS 4

typedef struct Monado3DWindowLayer {
    XrSwapchain swapchain;      // Overlay swapchain handle
    uint32_t swapchain_width;
    uint32_t swapchain_height;
    float x, y, width, height;  // Fractional window coordinates [0..1]
    float disparity;
    uint8_t active;
} Monado3DWindowLayer;

// --- Global shared state ---

typedef struct Monado3DState {
    // Double-buffered tunables: write index toggled by game thread
    Monado3DTunables tunables[2];
    volatile int tunables_read_idx;

    // Double-buffered scene transform: game thread writes, render thread reads
    Monado3DSceneTransform scene_transform[2];
    volatile int scene_transform_read_idx;

    // Display info (written once during init)
    Monado3DDisplayInfo display_info;

    // Eye positions (updated each frame from render thread)
    Monado3DEyePositions eye_positions[2];
    volatile int eyes_read_idx;

    // Window handle for session creation
    void *window_handle; // HWND on Win32, NSView* on macOS

    // Window-space overlay layers
    Monado3DWindowLayer window_layers[MONADO3D_MAX_WINDOW_LAYERS];
    int window_layer_count;

    // Readback state
    uint8_t *readback_pixels;
    uint32_t readback_width;
    uint32_t readback_height;
    volatile int readback_ready;

    // Extension support flags
    uint8_t has_display_info_ext;
    uint8_t has_win32_window_ext;
    uint8_t has_macos_window_ext;
    uint8_t has_display_mode_ext;

    // Function pointers for display mode switching
    PFN_xrRequestDisplayModeEXT pfn_request_display_mode;
} Monado3DState;

// Get the global shared state singleton.
Monado3DState *monado3d_get_state(void);

// Initialize shared state to defaults.
void monado3d_state_init(void);

// Set tunables from game thread (double-buffer swap).
void monado3d_state_set_tunables(const Monado3DTunables *t);

// Read current tunables from render thread.
Monado3DTunables monado3d_state_get_tunables(void);

// Update eye positions from render thread.
void monado3d_state_set_eye_positions(const XrVector3f *left, const XrVector3f *right, uint8_t tracked);

// Read eye positions from game thread.
Monado3DEyePositions monado3d_state_get_eye_positions(void);

// Set scene transform from game thread (double-buffer swap).
void monado3d_state_set_scene_transform(const Monado3DSceneTransform *t);

// Read scene transform from render thread.
Monado3DSceneTransform monado3d_state_get_scene_transform(void);

#ifdef __cplusplus
}
#endif
