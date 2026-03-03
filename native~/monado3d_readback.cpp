// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "monado3d_readback.h"
#include "monado3d_shared_state.h"

#include <stdlib.h>
#include <string.h>

void
monado3d_readback_callback(const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata)
{
	(void)userdata;

	Monado3DState *state = monado3d_get_state();

	// Reallocate if dimensions changed
	if (state->readback_width != width || state->readback_height != height) {
		monado3d_readback_alloc(width, height);
	}

	if (state->readback_pixels != nullptr) {
		uint32_t size = width * height * 4; // RGBA
		memcpy(state->readback_pixels, pixels, size);
		state->readback_ready = 1;
	}
}

void
monado3d_readback_alloc(uint32_t width, uint32_t height)
{
	Monado3DState *state = monado3d_get_state();

	monado3d_readback_free();

	uint32_t size = width * height * 4; // RGBA
	state->readback_pixels = (uint8_t *)malloc(size);
	state->readback_width = width;
	state->readback_height = height;
	state->readback_ready = 0;
}

void
monado3d_readback_free(void)
{
	Monado3DState *state = monado3d_get_state();

	if (state->readback_pixels != nullptr) {
		free(state->readback_pixels);
		state->readback_pixels = nullptr;
	}
	state->readback_width = 0;
	state->readback_height = 0;
	state->readback_ready = 0;
}
