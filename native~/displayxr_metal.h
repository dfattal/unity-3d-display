// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// IOSurface helper for zero-copy GPU texture sharing on macOS.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create a shared IOSurface (BGRA8, 4 bytes/pixel) and store it in shared state.
/// @return 1 on success, 0 on failure.
int displayxr_metal_create_shared_surface(uint32_t width, uint32_t height);

/// Release the shared IOSurface.
void displayxr_metal_destroy_shared_surface(void);

/// Get the IOSurfaceRef pointer (for passing to the runtime binding struct).
/// @return IOSurfaceRef cast to void*, or NULL if none exists.
void *displayxr_metal_get_iosurface(void);

/// Get the MTLTexture* backed by the shared IOSurface.
/// This is the pointer Unity's CreateExternalTexture needs.
void *displayxr_metal_get_texture(void);

/// Get the app's main window NSView (for passing to the cocoa window binding).
/// Tries mainWindow, keyWindow, then first visible window.
/// @return NSView* cast to void*, or NULL if no window found.
void *displayxr_get_app_main_view(void);

#ifdef __cplusplus
}
#endif
