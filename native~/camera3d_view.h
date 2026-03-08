// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric stereo view math for 3D displays
 *
 * Companion to display3d_view.h. Instead of scaling a physical display into
 * virtual app-space, the app defines a virtual camera (position, orientation,
 * vFOV) and eye tracking data produces per-eye asymmetric frustum views.
 *
 * Both abstractions share the same double-duty pattern: a single post-IPD/parallax
 * eye displacement feeds both the view matrix and the projection.
 *
 * The tunables struct takes half_tan_vfov = tan(vFOV/2) rather than the angle:
 *   - Zoom is trivial: half_tan_vfov / zoom_factor (no trig round-trip)
 *   - The math operates natively in tangent-space
 *   - Composable with any tangent-space operation
 *
 * Matrix convention: all output matrices are column-major (OpenGL/Vulkan/Metal).
 * DirectX callers should transpose when loading into row-major XMMATRIX.
 */

#pragma once

#include "display3d_view.h" // reuse Display3DScreen, display3d_apply_eye_factors

#ifdef __cplusplus
extern "C" {
#endif

// --- Structs ---

typedef struct Camera3DTunables {
	float ipd_factor;                //!< [0, 1] — scales inter-eye distance (0=mono, 1=full)
	float parallax_factor;           //!< [0, 1] — lerps eye center toward nominal (0=no tracking, 1=full)
	float inv_convergence_distance;  //!< 1/convergence_dist (1/meters)
	float half_tan_vfov;             //!< tan(vFOV/2) — divide by zoom at call site
} Camera3DTunables;

typedef struct Camera3DStereoView {
	float view_matrix[16];       //!< Column-major 4x4 (per-eye: eye displaced in world space)
	float projection_matrix[16]; //!< Column-major 4x4 asymmetric frustum (per-eye)
	XrFovf fov;                  //!< Asymmetric FOV angles in radians (per-eye)
	XrVector3f eye_world;        //!< Eye position in world space
	XrQuaternionf orientation;   //!< Camera orientation (same for both eyes)
} Camera3DStereoView;

// --- Functions ---

/*!
 * All-in-one: compute camera-centric stereo view+projection from raw eye tracking data.
 *
 * Pipeline:
 *   1. Apply IPD factor + parallax factor (reuses display3d_apply_eye_factors)
 *   2. Compute eye_local = processed - (0, 0, nominal_z)  (displacement from screen plane)
 *   3. Transform eye_local to world space via camera_pose
 *   4. Build view matrix from world-space eye + camera orientation
 *   5. Scale eye_local by inv_convergence_distance for projection shifts
 *   6. Compute asymmetric tangent half-angles from half_tan_vfov + aspect ratio
 *   7. Build projection matrix from tangent half-angles + near/far
 *   8. Convert tangents to XrFovf angles
 *
 * @param raw_left       Raw left eye in DISPLAY space (from xrLocateViews)
 * @param raw_right      Raw right eye in DISPLAY space (from xrLocateViews)
 * @param nominal_viewer Nominal viewer position in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions (used for aspect ratio)
 * @param tunables       Camera tunables (or NULL for defaults)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out_left       Output left eye view
 * @param out_right      Output right eye view
 */
void
camera3d_compute_stereo_views(const XrVector3f *raw_left,
                              const XrVector3f *raw_right,
                              const XrVector3f *nominal_viewer,
                              const Display3DScreen *screen,
                              const Camera3DTunables *tunables,
                              const XrPosef *camera_pose,
                              float near_z,
                              float far_z,
                              Camera3DStereoView *out_left,
                              Camera3DStereoView *out_right);

/*!
 * Default camera tunables: ipd=1, parallax=1, invd=1, half_tan=tan(18deg) (~36deg vFOV).
 */
Camera3DTunables
camera3d_default_tunables(void);

#ifdef __cplusplus
}
#endif
