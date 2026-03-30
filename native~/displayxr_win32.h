// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Win32 window detection for built app compositor output.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Get Unity's top-level main window HWND.
/// On first call: finds Unity's window and subclasses it for resize tracking.
/// On subsequent calls: returns the cached HWND.
/// @return HWND cast to void*, or NULL if no window found.
void *displayxr_get_app_main_view(void);

#ifdef __cplusplus
}
#endif
