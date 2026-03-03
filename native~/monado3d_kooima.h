// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Kooima asymmetric frustum projection for 3D displays.
// Ported from test_apps/common/xr_session_common.cpp.

#pragma once

#include <openxr/openxr.h>
#include "monado3d_shared_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Compute Kooima asymmetric frustum and return as XrFovf angles.
/// @param eye_pos Eye position in DISPLAY space (from xrLocateViews).
/// @param screen_width_m Physical display width in meters.
/// @param screen_height_m Physical display height in meters.
/// @return XrFovf with asymmetric frustum angles (radians).
XrFovf monado3d_compute_kooima_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m);

/// Apply scene transform to raw eye positions.
/// This is the "parent camera pose" from the Unity scene hierarchy.
/// Mirrors the test app's player transform: rotation, zoom, position offset.
/// Applied BEFORE tunables in the chain: raw → scene transform → tunables → Kooima.
void monado3d_apply_scene_transform(const XrVector3f *raw_left,
                                    const XrVector3f *raw_right,
                                    const Monado3DSceneTransform *xform,
                                    XrVector3f *out_left,
                                    XrVector3f *out_right);

/// Apply tunables to raw eye position from xrLocateViews.
/// @param raw_left Raw left eye position in DISPLAY space.
/// @param raw_right Raw right eye position in DISPLAY space.
/// @param tunables Current tunable parameters.
/// @param display_info Display physical properties.
/// @param out_left Output modified left eye position.
/// @param out_right Output modified right eye position.
void monado3d_apply_tunables(const XrVector3f *raw_left,
                             const XrVector3f *raw_right,
                             const Monado3DTunables *tunables,
                             const Monado3DDisplayInfo *display_info,
                             XrVector3f *out_left,
                             XrVector3f *out_right);

/// Compute screen extents for camera-centric mode.
/// @param convergence_distance Distance to virtual screen plane (meters).
/// @param fov_override Field of view override (radians, 0 = compute from display).
/// @param display_info Display physical properties.
/// @param out_width Output virtual screen width (meters).
/// @param out_height Output virtual screen height (meters).
void monado3d_camera_centric_extents(float convergence_distance,
                                     float fov_override,
                                     const Monado3DDisplayInfo *display_info,
                                     float *out_width,
                                     float *out_height);

#ifdef __cplusplus
}
#endif
