// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "monado3d_kooima.h"
#include "display3d_view.h"
#include <math.h>

// Quaternion-rotate a vector: v' = q * v * q^-1
static XrVector3f
quat_rotate(const float q[4], XrVector3f v)
{
	// q = (x, y, z, w)
	float qx = q[0], qy = q[1], qz = q[2], qw = q[3];

	// t = 2 * cross(q.xyz, v)
	float tx = 2.0f * (qy * v.z - qz * v.y);
	float ty = 2.0f * (qz * v.x - qx * v.z);
	float tz = 2.0f * (qx * v.y - qy * v.x);

	// v' = v + w*t + cross(q.xyz, t)
	XrVector3f out;
	out.x = v.x + qw * tx + (qy * tz - qz * ty);
	out.y = v.y + qw * ty + (qz * tx - qx * tz);
	out.z = v.z + qw * tz + (qx * ty - qy * tx);
	return out;
}

void
monado3d_apply_scene_transform(const XrVector3f *raw_left,
                               const XrVector3f *raw_right,
                               const Monado3DSceneTransform *xform,
                               XrVector3f *out_left,
                               XrVector3f *out_right)
{
	if (!xform->enabled) {
		*out_left = *raw_left;
		*out_right = *raw_right;
		return;
	}

	// Apply zoom: scale eye positions (closer zoom = stronger depth)
	// This mirrors the test app's: localPos = localPos / zoomScale
	float inv_zoom = 1.0f / xform->zoom_scale;

	XrVector3f left_scaled = {raw_left->x * inv_zoom, raw_left->y * inv_zoom, raw_left->z * inv_zoom};
	XrVector3f right_scaled = {raw_right->x * inv_zoom, raw_right->y * inv_zoom, raw_right->z * inv_zoom};

	// Apply rotation: worldPos = orientation * scaledPos
	XrVector3f left_rotated = quat_rotate(xform->orientation, left_scaled);
	XrVector3f right_rotated = quat_rotate(xform->orientation, right_scaled);

	// Apply translation: worldPos += position
	out_left->x = left_rotated.x + xform->position[0];
	out_left->y = left_rotated.y + xform->position[1];
	out_left->z = left_rotated.z + xform->position[2];

	out_right->x = right_rotated.x + xform->position[0];
	out_right->y = right_rotated.y + xform->position[1];
	out_right->z = right_rotated.z + xform->position[2];
}

XrFovf
monado3d_compute_kooima_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m)
{
	// Delegate to canonical display3d_view library
	return display3d_compute_fov(eye_pos, screen_width_m, screen_height_m);
}

void
monado3d_apply_tunables(const XrVector3f *raw_left,
                        const XrVector3f *raw_right,
                        const Monado3DTunables *tunables,
                        const Monado3DDisplayInfo *display_info,
                        XrVector3f *out_left,
                        XrVector3f *out_right)
{
	// Delegate IPD + parallax to canonical library.
	// The library lerps the eye center toward nominal on all XYZ axes
	// (previously this was X/Y-only parallax + Z-only perspective).
	XrVector3f nominal = {
	    display_info->nominal_viewer_x,
	    display_info->nominal_viewer_y,
	    display_info->nominal_viewer_z,
	};
	display3d_apply_eye_factors(raw_left, raw_right, &nominal,
	                            tunables->ipd_factor, tunables->parallax_factor,
	                            out_left, out_right);

	// Apply perspective factor: scale all XYZ (matches test apps)
	out_left->x *= tunables->perspective_factor;
	out_left->y *= tunables->perspective_factor;
	out_left->z *= tunables->perspective_factor;

	out_right->x *= tunables->perspective_factor;
	out_right->y *= tunables->perspective_factor;
	out_right->z *= tunables->perspective_factor;
}

void
monado3d_camera_centric_extents(float convergence_distance,
                                float fov_override,
                                const Monado3DDisplayInfo *display_info,
                                float *out_width,
                                float *out_height)
{
	if (fov_override > 0.0f) {
		// Compute virtual screen extents from FOV and convergence distance
		float half_fov = fov_override * 0.5f;
		float half_w = convergence_distance * tanf(half_fov);
		float aspect = display_info->display_width_meters / display_info->display_height_meters;
		*out_width = half_w * 2.0f;
		*out_height = *out_width / aspect;
	} else {
		// Scale physical display extents by convergence/nominal ratio
		float nominal_z = display_info->nominal_viewer_z;
		if (nominal_z <= 0.001f) {
			nominal_z = 0.65f;
		}
		float ratio = convergence_distance / nominal_z;
		*out_width = display_info->display_width_meters * ratio;
		*out_height = display_info->display_height_meters * ratio;
	}
}
