// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// OpenXR function interception for Unity's OpenXR Feature hook mechanism.

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform export macros
#if defined(_WIN32)
#define MONADO3D_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define MONADO3D_EXPORT __attribute__((visibility("default")))
#else
#define MONADO3D_EXPORT
#endif

/// Called by Unity's OpenXR Feature via HookGetInstanceProcAddr.
/// Stores the next xrGetInstanceProcAddr in the chain and returns our interceptor.
/// @param next The next xrGetInstanceProcAddr function pointer in the chain.
/// @return Our interceptor function pointer (cast to PFN_xrVoidFunction for C# IntPtr).
MONADO3D_EXPORT XrResult monado3d_hook_xrGetInstanceProcAddr(XrInstance instance,
                                                             const char *name,
                                                             PFN_xrVoidFunction *function);

/// Install the hook chain. Called once from C# with the next-in-chain function pointer.
/// @param next_gipa The next xrGetInstanceProcAddr in Unity's hook chain.
/// @return Our hook function pointer as PFN_xrVoidFunction (for C# IntPtr).
MONADO3D_EXPORT PFN_xrVoidFunction monado3d_install_hooks(PFN_xrGetInstanceProcAddr next_gipa);

// --- P/Invoke exports for C# ---

MONADO3D_EXPORT void monado3d_set_tunables(float ipd_factor,
                                           float parallax_factor,
                                           float perspective_factor,
                                           float scale_factor,
                                           float convergence_distance,
                                           float fov_override,
                                           int camera_centric);

MONADO3D_EXPORT void monado3d_get_display_info(float *display_width_m,
                                               float *display_height_m,
                                               uint32_t *pixel_width,
                                               uint32_t *pixel_height,
                                               float *nominal_x,
                                               float *nominal_y,
                                               float *nominal_z,
                                               float *scale_x,
                                               float *scale_y,
                                               int *supports_mode_switch,
                                               int *is_valid);

MONADO3D_EXPORT void monado3d_get_eye_positions(float *lx,
                                                float *ly,
                                                float *lz,
                                                float *rx,
                                                float *ry,
                                                float *rz,
                                                int *is_tracked);

MONADO3D_EXPORT void monado3d_set_scene_transform(float pos_x,
                                                   float pos_y,
                                                   float pos_z,
                                                   float ori_x,
                                                   float ori_y,
                                                   float ori_z,
                                                   float ori_w,
                                                   float zoom_scale,
                                                   int enabled);

MONADO3D_EXPORT void monado3d_set_window_handle(void *handle);

MONADO3D_EXPORT int monado3d_request_display_mode(int mode_3d);

MONADO3D_EXPORT void monado3d_get_readback(uint8_t **pixels,
                                           uint32_t *width,
                                           uint32_t *height,
                                           int *ready);

#ifdef __cplusplus
}
#endif
