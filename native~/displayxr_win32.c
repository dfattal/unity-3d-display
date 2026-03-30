// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Win32 window detection for built app compositor output.
// Finds Unity's top-level main window and subclasses it for resize tracking.
// The HWND is passed to the runtime via XR_EXT_win32_window_binding.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include "displayxr_hooks.h"

static HWND s_main_hwnd = NULL;
static WNDPROC s_original_wndproc = NULL;

// Subclassed wndproc for Unity's main window — tracks resize and screen position
static LRESULT CALLBACK
main_wnd_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_SIZE) {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		if (w > 0 && h > 0) {
			// Query window screen position for window-relative Kooima
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			displayxr_set_viewport_size_native(
				(uint32_t)w, (uint32_t)h,
				(int32_t)client_origin.x, (int32_t)client_origin.y);
		}
	}
	return CallWindowProcW(s_original_wndproc, hwnd, msg, wParam, lParam);
}

void *
displayxr_get_app_main_view(void)
{
	// Return cached HWND if still valid
	if (s_main_hwnd != NULL && IsWindow(s_main_hwnd))
		return (void *)s_main_hwnd;

	// Find Unity's main window: the foreground window belonging to our process
	DWORD our_pid = GetCurrentProcessId();
	HWND unity_hwnd = NULL;

	// Try foreground window first
	HWND fg = GetForegroundWindow();
	if (fg != NULL) {
		DWORD fg_pid = 0;
		GetWindowThreadProcessId(fg, &fg_pid);
		if (fg_pid == our_pid)
			unity_hwnd = fg;
	}

	// Fallback: enumerate top-level windows for our process
	if (unity_hwnd == NULL) {
		HWND hwnd = NULL;
		while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)) != NULL) {
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == our_pid && IsWindowVisible(hwnd)) {
				// Skip console windows — look for a window with a client area
				RECT rc;
				if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 100) {
					unity_hwnd = hwnd;
					break;
				}
			}
		}
	}

	if (unity_hwnd == NULL) {
		fprintf(stderr, "[DisplayXR] No Unity main window found\n");
		return NULL;
	}

	// Subclass Unity's window to track resize events and screen position
	s_original_wndproc = (WNDPROC)SetWindowLongPtrW(
	    unity_hwnd, GWLP_WNDPROC, (LONG_PTR)main_wnd_subclass_proc);

	// Push initial viewport size
	RECT client_rc;
	GetClientRect(unity_hwnd, &client_rc);
	int w = client_rc.right - client_rc.left;
	int h = client_rc.bottom - client_rc.top;
	if (w > 0 && h > 0) {
		POINT client_origin = {0, 0};
		ClientToScreen(unity_hwnd, &client_origin);
		displayxr_set_viewport_size_native(
			(uint32_t)w, (uint32_t)h,
			(int32_t)client_origin.x, (int32_t)client_origin.y);
	}

	s_main_hwnd = unity_hwnd;

	fprintf(stderr, "[DisplayXR] Found main window HWND %p (%dx%d)\n",
	        (void *)unity_hwnd, w, h);

	return (void *)s_main_hwnd;
}

#endif // _WIN32
