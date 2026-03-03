// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unified display-centric stereo view math for 3D displays
 *
 * Canonical implementation of Kooima asymmetric frustum projection, view matrix
 * construction, and stereo eye factor processing. Any project that needs
 * display-centric stereo views should use this library instead of reimplementing
 * the math. Pure C, depends only on <openxr/openxr.h> and <math.h>.
 *
 * Reference: Robert Kooima, "Generalized Perspective Projection" (2009)
 *
 * Matrix convention: all output matrices are column-major (OpenGL/Vulkan/Metal).
 * DirectX callers should transpose when loading into row-major XMMATRIX.
 */

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Structs ---

typedef struct Display3DTunables {
	float ipd_factor;         //!< [0, 1] — scales inter-eye distance (0=mono, 1=full)
	float parallax_factor;    //!< [0, 1] — lerps eye center toward nominal (0=no tracking, 1=full)
	float perspective_factor; //!< [0.1, 10] — scales eye XYZ in view+projection (not screen)
	float scale_factor;       //!< [0.1, 10] — scales eye XYZ AND screenW/H (cancels in projection = zoom)
} Display3DTunables;

typedef struct Display3DScreen {
	float width_m;  //!< Physical screen width (meters)
	float height_m; //!< Physical screen height (meters)
} Display3DScreen;

typedef struct Display3DStereoView {
	float view_matrix[16];       //!< Column-major 4x4 view matrix
	float projection_matrix[16]; //!< Column-major 4x4 projection matrix
	XrFovf fov;                  //!< Equivalent asymmetric FOV angles (radians)
	XrVector3f eye_display;      //!< Modified eye position in display space (after all factors)
	XrVector3f eye_world;        //!< Eye position in world space (after display pose transform)
} Display3DStereoView;

// --- Functions ---

/*!
 * All-in-one: compute stereo view+projection from raw eye tracking data.
 *
 * Pipeline:
 *   1. Apply IPD factor (scale inter-eye vector, keep center fixed)
 *   2. Apply parallax factor (lerp center toward nominal viewer)
 *   3. Apply perspective+scale to eye XYZ (view matrix + Kooima eye)
 *   4. Apply scale to screen W/H (Kooima screen dims)
 *   5. Transform display-space eye -> world-space via display_pose
 *   6. Build view matrix from world-space eye + display orientation
 *   7. Build Kooima projection matrix from display-space scaled eye + scaled screen
 *   8. Compute FOV angles from same
 *
 * @param raw_left       Raw left eye in DISPLAY space (from xrLocateViews)
 * @param raw_right      Raw right eye in DISPLAY space (from xrLocateViews)
 * @param nominal_viewer Nominal viewer position in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions
 * @param tunables       Stereo factors (or NULL for defaults: all 1.0)
 * @param display_pose   Display pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out_left       Output left eye view
 * @param out_right      Output right eye view
 */
void
display3d_compute_stereo_views(const XrVector3f *raw_left,
                               const XrVector3f *raw_right,
                               const XrVector3f *nominal_viewer,
                               const Display3DScreen *screen,
                               const Display3DTunables *tunables,
                               const XrPosef *display_pose,
                               float near_z,
                               float far_z,
                               Display3DStereoView *out_left,
                               Display3DStereoView *out_right);

/*!
 * Compute Kooima FOV angles only (no matrices). Useful for runtime-side
 * computation where the app builds its own projection.
 *
 * @param eye_pos          Eye position in display space (after all factors)
 * @param screen_width_m   Screen width in meters
 * @param screen_height_m  Screen height in meters
 * @return XrFovf with asymmetric frustum angles (radians)
 */
XrFovf
display3d_compute_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m);

/*!
 * Compute Kooima projection matrix (column-major float[16]).
 *
 * @param eye_pos          Eye position in display space (after all factors)
 * @param screen_width_m   Screen width in meters
 * @param screen_height_m  Screen height in meters
 * @param near_z           Near clip plane distance
 * @param far_z            Far clip plane distance
 * @param out_matrix       Output float[16], column-major
 */
void
display3d_compute_projection(XrVector3f eye_pos,
                             float screen_width_m,
                             float screen_height_m,
                             float near_z,
                             float far_z,
                             float *out_matrix);

/*!
 * Apply eye factors only (IPD + parallax). Useful when caller builds
 * its own view matrix but wants consistent eye processing.
 *
 * @param raw_left        Raw left eye position
 * @param raw_right       Raw right eye position
 * @param nominal_viewer  Nominal viewer position (or NULL for {0,0,0.5})
 * @param ipd_factor      IPD scaling factor [0, 1]
 * @param parallax_factor Parallax lerp factor [0, 1]
 * @param out_left        Output processed left eye position
 * @param out_right       Output processed right eye position
 */
void
display3d_apply_eye_factors(const XrVector3f *raw_left,
                            const XrVector3f *raw_right,
                            const XrVector3f *nominal_viewer,
                            float ipd_factor,
                            float parallax_factor,
                            XrVector3f *out_left,
                            XrVector3f *out_right);

/*!
 * Default tunables (all factors = 1.0).
 */
Display3DTunables
display3d_default_tunables(void);

#ifdef __cplusplus
}
#endif
