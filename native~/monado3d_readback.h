// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Readback buffer management for offscreen editor preview.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Readback callback invoked by Monado runtime (macOS offscreen mode).
/// Copies pixel data into the shared state readback buffer.
void monado3d_readback_callback(const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

/// Allocate the readback buffer for given dimensions.
void monado3d_readback_alloc(uint32_t width, uint32_t height);

/// Free the readback buffer.
void monado3d_readback_free(void);

#ifdef __cplusplus
}
#endif
