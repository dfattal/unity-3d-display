// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Win32 window overlay for built app compositor output.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Get or create the overlay child HWND on top of Unity's main window.
/// On first call: finds Unity's window, creates a transparent child window.
/// On subsequent calls: returns the existing overlay HWND.
/// @return HWND cast to void*, or NULL if no window found.
void *displayxr_get_app_main_view(void);

#ifdef __cplusplus
}
#endif
