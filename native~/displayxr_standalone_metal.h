// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// IOSurface helper for the standalone preview session (macOS).
// Separate from the hook chain's IOSurface to allow independent lifecycles.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create a standalone IOSurface + MTLTexture for the preview session.
/// @return 1 on success, 0 on failure.
int displayxr_sa_metal_create(uint32_t width, uint32_t height);

/// Destroy the standalone IOSurface + MTLTexture.
void displayxr_sa_metal_destroy(void);

/// Get the IOSurfaceRef for the standalone session.
void *displayxr_sa_metal_get_iosurface(void);

/// Get the MTLTexture* for Unity's CreateExternalTexture.
void *displayxr_sa_metal_get_texture(void);

/// Get (or lazily create) an MTLCommandQueue for the standalone session.
/// Required for XrGraphicsBindingMetalKHR.
void *displayxr_sa_metal_get_command_queue(void);

#ifdef __cplusplus
}
#endif
