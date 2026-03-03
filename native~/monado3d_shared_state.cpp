// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "monado3d_shared_state.h"
#include <string.h>
#include <atomic>

static Monado3DState s_state = {};
static std::atomic<int> s_tunables_write_idx{1};
static std::atomic<int> s_eyes_write_idx{1};
static std::atomic<int> s_scene_transform_write_idx{1};

Monado3DState *
monado3d_get_state(void)
{
	return &s_state;
}

void
monado3d_state_init(void)
{
	memset(&s_state, 0, sizeof(s_state));

	// Default tunables: all factors at 1.0
	for (int i = 0; i < 2; i++) {
		s_state.tunables[i].ipd_factor = 1.0f;
		s_state.tunables[i].parallax_factor = 1.0f;
		s_state.tunables[i].perspective_factor = 1.0f;
		s_state.tunables[i].scale_factor = 1.0f;
		s_state.tunables[i].convergence_distance = 0.0f;
		s_state.tunables[i].fov_override = 0.0f;
		s_state.tunables[i].camera_centric = 0;
	}

	s_state.tunables_read_idx = 0;
	s_tunables_write_idx.store(1, std::memory_order_relaxed);

	s_state.eyes_read_idx = 0;
	s_eyes_write_idx.store(1, std::memory_order_relaxed);

	// Default scene transform: identity, no zoom, disabled
	for (int i = 0; i < 2; i++) {
		s_state.scene_transform[i].position[0] = 0.0f;
		s_state.scene_transform[i].position[1] = 0.0f;
		s_state.scene_transform[i].position[2] = 0.0f;
		s_state.scene_transform[i].orientation[0] = 0.0f;
		s_state.scene_transform[i].orientation[1] = 0.0f;
		s_state.scene_transform[i].orientation[2] = 0.0f;
		s_state.scene_transform[i].orientation[3] = 1.0f; // w=1 identity
		s_state.scene_transform[i].zoom_scale = 1.0f;
		s_state.scene_transform[i].enabled = 0;
	}
	s_state.scene_transform_read_idx = 0;
	s_scene_transform_write_idx.store(1, std::memory_order_relaxed);
}

void
monado3d_state_set_tunables(const Monado3DTunables *t)
{
	// Write to the non-read buffer, then swap
	int write_idx = s_tunables_write_idx.load(std::memory_order_relaxed);
	s_state.tunables[write_idx] = *t;

	// Swap: make this the read buffer, old read becomes write
	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.tunables_read_idx = old_read;
	s_tunables_write_idx.store(new_write, std::memory_order_release);
}

Monado3DTunables
monado3d_state_get_tunables(void)
{
	int idx = s_state.tunables_read_idx;
	return s_state.tunables[idx];
}

void
monado3d_state_set_eye_positions(const XrVector3f *left, const XrVector3f *right, uint8_t tracked)
{
	int write_idx = s_eyes_write_idx.load(std::memory_order_relaxed);
	s_state.eye_positions[write_idx].left_eye = *left;
	s_state.eye_positions[write_idx].right_eye = *right;
	s_state.eye_positions[write_idx].is_tracked = tracked;

	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.eyes_read_idx = old_read;
	s_eyes_write_idx.store(new_write, std::memory_order_release);
}

Monado3DEyePositions
monado3d_state_get_eye_positions(void)
{
	int idx = s_state.eyes_read_idx;
	return s_state.eye_positions[idx];
}

void
monado3d_state_set_scene_transform(const Monado3DSceneTransform *t)
{
	int write_idx = s_scene_transform_write_idx.load(std::memory_order_relaxed);
	s_state.scene_transform[write_idx] = *t;

	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.scene_transform_read_idx = old_read;
	s_scene_transform_write_idx.store(new_write, std::memory_order_release);
}

Monado3DSceneTransform
monado3d_state_get_scene_transform(void)
{
	int idx = s_state.scene_transform_read_idx;
	return s_state.scene_transform[idx];
}
