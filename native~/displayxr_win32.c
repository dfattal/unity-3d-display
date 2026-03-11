// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Win32 overlay child window for built app compositor output.
// Creates a transparent child HWND on top of Unity's main window.
// The D3D11 compositor creates its DXGI swap chain on this child HWND,
// presenting on top of Unity's rendering.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

static HWND s_overlay_hwnd = NULL;
static WNDPROC s_original_wndproc = NULL;
static const wchar_t OVERLAY_CLASS_NAME[] = L"DisplayXROverlay";
static int s_class_registered = 0;

static LRESULT CALLBACK
overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_NCHITTEST:
		// Pass all input through to the parent window
		return HTTRANSPARENT;
	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

// Subclassed wndproc for Unity's parent window — resizes the overlay child
static LRESULT CALLBACK
parent_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_SIZE && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		SetWindowPos(s_overlay_hwnd, HWND_TOP, 0, 0, w, h, SWP_NOZORDER);
	}
	return CallWindowProcW(s_original_wndproc, hwnd, msg, wParam, lParam);
}

static int
register_overlay_class(void)
{
	if (s_class_registered)
		return 1;

	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = overlay_wnd_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = OVERLAY_CLASS_NAME;
	wc.style = CS_OWNDC;

	if (RegisterClassExW(&wc) == 0) {
		fprintf(stderr, "[DisplayXR] Failed to register overlay window class: %lu\n",
		        GetLastError());
		return 0;
	}

	s_class_registered = 1;
	return 1;
}

void *
displayxr_get_app_main_view(void)
{
	// Return existing overlay if still valid
	if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd))
		return (void *)s_overlay_hwnd;

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

	// Register window class
	if (!register_overlay_class())
		return NULL;

	// Get client area size
	RECT client_rc;
	GetClientRect(unity_hwnd, &client_rc);
	int w = client_rc.right - client_rc.left;
	int h = client_rc.bottom - client_rc.top;

	// Create child window overlay
	// WS_CHILD: child of Unity's window (moves/resizes with parent)
	// WS_VISIBLE: immediately visible
	// WS_EX_TRANSPARENT: input passes through to parent
	s_overlay_hwnd = CreateWindowExW(
	    WS_EX_TRANSPARENT,
	    OVERLAY_CLASS_NAME,
	    L"DisplayXR Overlay",
	    WS_CHILD | WS_VISIBLE,
	    0, 0, w, h,
	    unity_hwnd,
	    NULL,
	    GetModuleHandleW(NULL),
	    NULL);

	if (s_overlay_hwnd == NULL) {
		fprintf(stderr, "[DisplayXR] Failed to create overlay window: %lu\n",
		        GetLastError());
		return NULL;
	}

	// Subclass Unity's window to track resize events
	s_original_wndproc = (WNDPROC)SetWindowLongPtrW(
	    unity_hwnd, GWLP_WNDPROC, (LONG_PTR)parent_subclass_proc);

	fprintf(stderr, "[DisplayXR] Created overlay HWND (%dx%d) on Unity window %p\n",
	        w, h, (void *)unity_hwnd);

	return (void *)s_overlay_hwnd;
}

#endif // _WIN32
