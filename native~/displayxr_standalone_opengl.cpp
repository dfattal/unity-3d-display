// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenGL standalone graphics backend for the DisplayXR preview window.
// Windows: creates a GL 4.3+ core context via WGL; glCopyImageSubData for atlas blits.
// Linux: stub (no editor preview implementation).
// Shared texture interop with Unity requires WGL_NV_DX_interop — currently not wired,
// so get_shared_texture_native_ptr returns nullptr (preview texture unsupported for GL standalone).

#if defined(ENABLE_OPENGL) && !defined(__ANDROID__)

#include "displayxr_standalone_internal.h"

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>

// WGL extension function types
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);
#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef void (APIENTRY *PFNGLCOPYIMAGESUBDATAPROC)(
	GLuint srcName, GLenum srcTarget, GLint srcLevel,
	GLint srcX, GLint srcY, GLint srcZ,
	GLuint dstName, GLenum dstTarget, GLint dstLevel,
	GLint dstX, GLint dstY, GLint dstZ,
	GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);

typedef struct XrSwapchainImageOpenGLKHR {
	XrStructureType type;
	void           *next;
	uint32_t        image; // GL texture name
} XrSwapchainImageOpenGLKHR;

#define XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR    ((XrStructureType)1000023001)
#define XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR ((XrStructureType)1000023000)

typedef struct XrGraphicsBindingOpenGLWin32KHR {
	XrStructureType type;
	const void     *next;
	HDC             hDC;
	HGLRC           hGLRC;
} XrGraphicsBindingOpenGLWin32KHR;

#define GL_RGBA8 0x8058

#elif defined(__linux__)
#include <GL/gl.h>
#include <GL/glx.h>

typedef void (*PFNGLCOPYIMAGESUBDATAPROC)(
	GLuint srcName, GLenum srcTarget, GLint srcLevel,
	GLint srcX, GLint srcY, GLint srcZ,
	GLuint dstName, GLenum dstTarget, GLint dstLevel,
	GLint dstX, GLint dstY, GLint dstZ,
	GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);

typedef struct XrSwapchainImageOpenGLKHR {
	XrStructureType type;
	void           *next;
	uint32_t        image;
} XrSwapchainImageOpenGLKHR;

#define XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR    ((XrStructureType)1000023001)
#define XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR ((XrStructureType)1000023003)

#define GL_RGBA8 0x8058
#endif

// ---------------------------------------------------------------------------
// StandaloneOpenGLBackend
// ---------------------------------------------------------------------------
class StandaloneOpenGLBackend : public StandaloneGraphicsBackend {
public:
#if defined(_WIN32)
	HWND  dummy_hwnd = nullptr;
	HDC   dummy_hdc  = nullptr;
	HGLRC gl_context = nullptr;
	HDC   fw_hdc     = nullptr;
	HGLRC fw_glrc    = nullptr;
	HWND  fw_hwnd    = nullptr;

	PFNGLCOPYIMAGESUBDATAPROC pfn_copy_image = nullptr;

	// Session binding returned by build_session_binding
	XrGraphicsBindingOpenGLWin32KHR session_binding = {};
#elif defined(__linux__)
	Display    *xdisplay    = nullptr;
	GLXContext  glx_context = nullptr;
	GLXWindow   glx_window  = 0;
#endif

	// Atlas images (GL texture names)
	XrSwapchainImageOpenGLKHR atlas_images[SA_MAX_SWAPCHAIN_IMAGES] = {};

	// Atlas bridge texture (for blit)
	uint32_t atlas_bridge_tex = 0;
	uint32_t atlas_bridge_w = 0;
	uint32_t atlas_bridge_h = 0;

	bool create_device(XrInstance /*instance*/, XrSystemId /*system_id*/,
	                    PFN_xrGetInstanceProcAddr /*gipa*/) override
	{
#if defined(_WIN32)
		// Create a dummy window + legacy context to bootstrap WGL extensions
		WNDCLASSA wc = {};
		wc.style         = CS_OWNDC;
		wc.lpfnWndProc   = DefWindowProcA;
		wc.hInstance     = GetModuleHandle(nullptr);
		wc.lpszClassName = "DisplayXR_GL_Dummy";
		RegisterClassA(&wc);

		dummy_hwnd = CreateWindowA("DisplayXR_GL_Dummy", "", WS_POPUP,
		    0, 0, 1, 1, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
		if (!dummy_hwnd) {
			fprintf(stderr, "[DisplayXR-SA-GL] CreateWindow (dummy) failed\n");
			return false;
		}
		dummy_hdc = GetDC(dummy_hwnd);

		PIXELFORMATDESCRIPTOR pfd = {};
		pfd.nSize      = sizeof(pfd);
		pfd.nVersion   = 1;
		pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		int fmt = ChoosePixelFormat(dummy_hdc, &pfd);
		SetPixelFormat(dummy_hdc, fmt, &pfd);

		HGLRC legacy_ctx = wglCreateContext(dummy_hdc);
		if (!legacy_ctx) {
			fprintf(stderr, "[DisplayXR-SA-GL] wglCreateContext (legacy) failed\n");
			return false;
		}
		wglMakeCurrent(dummy_hdc, legacy_ctx);

		// Load wglCreateContextAttribsARB
		PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
		    (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
		if (!wglCreateContextAttribsARB) {
			fprintf(stderr, "[DisplayXR-SA-GL] wglCreateContextAttribsARB not available\n");
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(legacy_ctx);
			return false;
		}

		const int attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
			WGL_CONTEXT_MINOR_VERSION_ARB, 3,
			WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
			0
		};
		gl_context = wglCreateContextAttribsARB(dummy_hdc, nullptr, attribs);
		wglMakeCurrent(nullptr, nullptr);
		wglDeleteContext(legacy_ctx);

		if (!gl_context) {
			fprintf(stderr, "[DisplayXR-SA-GL] wglCreateContextAttribsARB failed (GL 4.3 not supported?)\n");
			return false;
		}
		wglMakeCurrent(dummy_hdc, gl_context);

		// Load glCopyImageSubData
		pfn_copy_image = (PFNGLCOPYIMAGESUBDATAPROC)wglGetProcAddress("glCopyImageSubData");
		if (!pfn_copy_image)
			fprintf(stderr, "[DisplayXR-SA-GL] glCopyImageSubData not available\n");

		wglMakeCurrent(nullptr, nullptr);
		fprintf(stderr, "[DisplayXR-SA-GL] GL 4.3 core context created\n");
		return true;
#elif defined(__linux__)
		// Linux: stub
		fprintf(stderr, "[DisplayXR-SA-GL] OpenGL standalone not implemented on Linux (no editor)\n");
		return false;
#else
		return false;
#endif
	}

	bool create_shared_texture(uint32_t /*width*/, uint32_t /*height*/) override
	{
		// WGL_NV_DX_interop needed for D3D interop on Windows — not implemented.
		// Return true (non-fatal) so the session can proceed without a shared texture.
		return true;
	}

	void destroy_shared_texture() override {}

	void *get_shared_texture_native_ptr() override
	{
		// Shared texture preview not implemented for GL standalone.
		return nullptr;
	}

	const void *build_session_binding(void * /*platform_window_handle*/, void * /*shared_texture_handle*/) override
	{
#if defined(_WIN32)
		if (!gl_context || !dummy_hdc) return nullptr;
		session_binding.type  = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
		session_binding.next  = nullptr;
		session_binding.hDC   = dummy_hdc;
		session_binding.hGLRC = gl_context;
		return &session_binding;
#else
		return nullptr;
#endif
	}

	bool enumerate_atlas_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages pfn, uint32_t count) override
	{
		for (uint32_t i = 0; i < count; i++) {
			atlas_images[i].type  = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
			atlas_images[i].next  = nullptr;
			atlas_images[i].image = 0;
		}
		XrResult result = pfn(swapchain, count, &count,
		    (XrSwapchainImageBaseHeader *)atlas_images);
		if (XR_FAILED(result)) {
			fprintf(stderr, "[DisplayXR-SA-GL] xrEnumerateSwapchainImages failed: %d\n", result);
			return false;
		}
		return true;
	}

	void *get_atlas_image(uint32_t index) override
	{
		// Return GL texture name as void*
		return (void *)(uintptr_t)atlas_images[index].image;
	}

	void create_atlas_bridge(uint32_t atlas_w, uint32_t atlas_h, void * /*unity_dev*/) override
	{
#if defined(_WIN32)
		if (!gl_context || !dummy_hdc) return;
		wglMakeCurrent(dummy_hdc, gl_context);
		glGenTextures(1, &atlas_bridge_tex);
		glBindTexture(0x0DE1 /*GL_TEXTURE_2D*/, atlas_bridge_tex);
		glTexImage2D(0x0DE1, 0, GL_RGBA8, (GLsizei)atlas_w, (GLsizei)atlas_h,
		             0, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, nullptr);
		glBindTexture(0x0DE1, 0);
		wglMakeCurrent(nullptr, nullptr);
		atlas_bridge_w = atlas_w;
		atlas_bridge_h = atlas_h;
		fprintf(stderr, "[DisplayXR-SA-GL] Atlas bridge texture: %ux%u GL=%u\n",
		        atlas_w, atlas_h, atlas_bridge_tex);
#else
		(void)atlas_w; (void)atlas_h;
#endif
	}

	void destroy_atlas_bridge() override
	{
#if defined(_WIN32)
		if (atlas_bridge_tex && gl_context && dummy_hdc) {
			wglMakeCurrent(dummy_hdc, gl_context);
			glDeleteTextures(1, &atlas_bridge_tex);
			wglMakeCurrent(nullptr, nullptr);
			atlas_bridge_tex = 0;
		}
#endif
	}

	void *get_atlas_bridge_unity_ptr() override
	{
		// WGL_NV_DX_interop needed for D3D interop — not implemented.
		return nullptr;
	}

	void blit_atlas(void * /*atlas_tex*/, uint32_t index) override
	{
#if defined(_WIN32)
		if (!pfn_copy_image || !gl_context || !dummy_hdc) return;
		uint32_t src = atlas_images[index].image;
		uint32_t dst = atlas_bridge_tex;
		if (!src || !dst) return;
		wglMakeCurrent(dummy_hdc, gl_context);
		pfn_copy_image(
		    src, 0x0DE1 /*GL_TEXTURE_2D*/, 0, 0, 0, 0,
		    dst, 0x0DE1,                   0, 0, 0, 0,
		    (GLsizei)atlas_bridge_w, (GLsizei)atlas_bridge_h, 1);
		glFinish();
		wglMakeCurrent(nullptr, nullptr);
#else
		(void)index;
#endif
	}

	bool fw_create_swapchain(void *hwnd, uint32_t /*w*/, uint32_t /*h*/) override
	{
#if defined(_WIN32)
		fw_hwnd = (HWND)hwnd;
		if (!fw_hwnd) return false;
		fw_hdc = GetDC(fw_hwnd);

		PIXELFORMATDESCRIPTOR pfd = {};
		pfd.nSize      = sizeof(pfd);
		pfd.nVersion   = 1;
		pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		int fmt = ChoosePixelFormat(fw_hdc, &pfd);
		SetPixelFormat(fw_hdc, fmt, &pfd);
		fw_glrc = wglCreateContext(fw_hdc);
		// Share lists between contexts so we can draw atlas_bridge_tex in fw_present
		if (gl_context && fw_glrc)
			wglShareLists(gl_context, fw_glrc);
		return fw_glrc != nullptr;
#else
		(void)hwnd;
		return false;
#endif
	}

	void fw_destroy_swapchain() override
	{
#if defined(_WIN32)
		if (fw_glrc) { wglDeleteContext(fw_glrc); fw_glrc = nullptr; }
		if (fw_hwnd && fw_hdc) { ReleaseDC(fw_hwnd, fw_hdc); fw_hdc = nullptr; fw_hwnd = nullptr; }
#endif
	}

	void fw_resize_swapchain_buffers(uint32_t /*w*/, uint32_t /*h*/) override
	{
		// No-op for GL — window resize is handled automatically
	}

	void fw_present(uint32_t /*sc_w*/, uint32_t /*sc_h*/) override
	{
#if defined(_WIN32)
		if (!fw_hdc || !fw_glrc) return;
		// SwapBuffers presents whatever is in the back buffer.
		SwapBuffers(fw_hdc);
#endif
	}

	void destroy() override
	{
		destroy_atlas_bridge();
		fw_destroy_swapchain();
#if defined(_WIN32)
		if (gl_context) {
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(gl_context);
			gl_context = nullptr;
		}
		if (dummy_hwnd && dummy_hdc) { ReleaseDC(dummy_hwnd, dummy_hdc); dummy_hdc = nullptr; }
		if (dummy_hwnd) { DestroyWindow(dummy_hwnd); dummy_hwnd = nullptr; }
#endif
	}
};

StandaloneGraphicsBackend *create_standalone_opengl_backend() { return new StandaloneOpenGLBackend(); }

#endif // defined(ENABLE_OPENGL) && !defined(__ANDROID__)
