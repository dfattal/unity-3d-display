// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric stereo view math for 3D displays
 *
 * Canonical implementation — see camera3d_view.h for API docs.
 */

#include "camera3d_view.h"
#include <math.h>
#include <string.h>

// ============================================================================
// Internal helpers (duplicated from display3d_view.c as static to avoid
// exposing internal symbols — ~60 lines total)
// ============================================================================

// Quaternion-rotate a vector: v' = q * v * q^-1
static XrVector3f
cam3d_quat_rotate(XrQuaternionf q, XrVector3f v)
{
	float tx = 2.0f * (q.y * v.z - q.z * v.y);
	float ty = 2.0f * (q.z * v.x - q.x * v.z);
	float tz = 2.0f * (q.x * v.y - q.y * v.x);

	XrVector3f out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

// Set a column-major 4x4 matrix to identity.
static void
cam3d_mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build rotation matrix (column-major) from quaternion.
static void
cam3d_mat4_from_quat(float *m, XrQuaternionf q)
{
	float qx = q.x, qy = q.y, qz = q.z, qw = q.w;

	cam3d_mat4_identity(m);
	m[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
	m[1] = 2.0f * (qx * qy + qz * qw);
	m[2] = 2.0f * (qx * qz - qy * qw);
	m[4] = 2.0f * (qx * qy - qz * qw);
	m[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
	m[6] = 2.0f * (qy * qz + qx * qw);
	m[8] = 2.0f * (qx * qz + qy * qw);
	m[9] = 2.0f * (qy * qz - qx * qw);
	m[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
}

// Build view matrix from camera pose and world-space eye position.
// viewMatrix = transpose(R) * translate(-eye_world)
static void
cam3d_build_view_matrix(float *out, XrQuaternionf orientation, XrVector3f eye_world)
{
	float rot[16];
	cam3d_mat4_from_quat(rot, orientation);

	// Transpose rotation (inverse of orthonormal matrix)
	float inv_rot[16];
	cam3d_mat4_identity(inv_rot);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot[j * 4 + i] = rot[i * 4 + j];

	// Translation: -eye_world
	float inv_trans[16];
	cam3d_mat4_identity(inv_trans);
	inv_trans[12] = -eye_world.x;
	inv_trans[13] = -eye_world.y;
	inv_trans[14] = -eye_world.z;

	// viewMatrix = inv_rot * inv_trans (column-major multiplication)
	float tmp[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += inv_rot[k * 4 + row] * inv_trans[col * 4 + k];
			}
			tmp[col * 4 + row] = sum;
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

// Build asymmetric frustum projection (column-major) from tangent half-angles.
static void
cam3d_build_projection_from_tangents(float tan_left,
                                     float tan_right,
                                     float tan_down,
                                     float tan_up,
                                     float near_z,
                                     float far_z,
                                     float *out)
{
	float left = -tan_left * near_z;
	float right = tan_right * near_z;
	float bottom = -tan_down * near_z;
	float top = tan_up * near_z;

	float w = right - left;
	float h = top - bottom;

	memset(out, 0, 16 * sizeof(float));
	out[0] = 2.0f * near_z / w;
	out[5] = 2.0f * near_z / h;
	out[8] = (right + left) / w;
	out[9] = (top + bottom) / h;
	out[10] = -(far_z + near_z) / (far_z - near_z);
	out[11] = -1.0f;
	out[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
}

// ============================================================================
// Public API
// ============================================================================

Camera3DTunables
camera3d_default_tunables(void)
{
	Camera3DTunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.inv_convergence_distance = 1.0f;
	t.half_tan_vfov = 0.32491969623f; // tan(18 deg) → 36° vFOV
	return t;
}

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
                              Camera3DStereoView *out_right)
{
	// Resolve defaults
	Camera3DTunables t = tunables ? *tunables : camera3d_default_tunables();

	XrQuaternionf cam_ori = {0, 0, 0, 1};
	XrVector3f cam_pos = {0, 0, 0};
	if (camera_pose) {
		cam_ori = camera_pose->orientation;
		cam_pos = camera_pose->position;
	}

	// Default nominal viewer
	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Step 1: Apply IPD and parallax factors (shared with display-centric path)
	XrVector3f processed[2];
	display3d_apply_eye_factors(raw_left, raw_right, nominal_viewer, t.ipd_factor, t.parallax_factor,
	                            &processed[0], &processed[1]);

	// Aspect ratio for horizontal FOV
	float aspect = screen->width_m / screen->height_m;
	float ro = t.half_tan_vfov * aspect; // horizontal half-tangent
	float uo = t.half_tan_vfov;          // vertical half-tangent
	float invd = t.inv_convergence_distance;

	// Process each eye
	Camera3DStereoView *outputs[2] = {out_left, out_right};
	for (int i = 0; i < 2; i++) {
		// Step 2: Compute eye_local = displacement from nominal screen plane
		XrVector3f eye_local;
		eye_local.x = processed[i].x;
		eye_local.y = processed[i].y;
		eye_local.z = processed[i].z - nom_z;

		// Step 3: Transform eye_local to world space via camera_pose
		XrVector3f eye_world = cam3d_quat_rotate(cam_ori, eye_local);
		eye_world.x += cam_pos.x;
		eye_world.y += cam_pos.y;
		eye_world.z += cam_pos.z;
		outputs[i]->eye_world = eye_world;

		// Step 4: Build view matrix from world-space eye + camera orientation
		cam3d_build_view_matrix(outputs[i]->view_matrix, cam_ori, eye_world);
		outputs[i]->orientation = cam_ori;

		// Step 5: Scale eye_local by inv_convergence_distance for projection shifts
		float dx = eye_local.x * invd;
		float dy = eye_local.y * invd;
		float dz = eye_local.z * invd;

		// Step 6-7: Compute asymmetric frustum tangent half-angles
		float denom = 1.0f + dz;
		float tan_right = (ro - dx) / denom;
		float tan_left = (ro + dx) / denom;
		float tan_up = (uo - dy) / denom;
		float tan_down = (uo + dy) / denom;

		// Step 8: Build projection matrix from tangent half-angles
		cam3d_build_projection_from_tangents(tan_left, tan_right, tan_down, tan_up, near_z, far_z,
		                                     outputs[i]->projection_matrix);

		// Step 9: Convert tangents to XrFovf (OpenXR signed angle convention)
		outputs[i]->fov.angleLeft = -atanf(tan_left);
		outputs[i]->fov.angleRight = atanf(tan_right);
		outputs[i]->fov.angleUp = atanf(tan_up);
		outputs[i]->fov.angleDown = -atanf(tan_down);
	}
}
