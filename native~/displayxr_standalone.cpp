// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Standalone OpenXR session for editor preview.
// Loads the DisplayXR runtime directly via xrNegotiateLoaderRuntimeInterface,
// bypassing Unity's OpenXR loader entirely. This gives us full control over
// the session lifecycle — no deferred destruction, no signal handlers, no
// teardown races with Unity's Game View repaint cycle.

#include "displayxr_standalone_internal.h"

// ============================================================================
// OpenXR loader negotiation types (from openxr_loader_negotiation.h)
// Defined inline to avoid header dependency issues.
// ============================================================================

#define XR_LOADER_INTERFACE_STRUCT_LOADER_INFO   1
#define XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST 3
#define XR_CURRENT_LOADER_RUNTIME_VERSION 1

typedef struct XrNegotiateLoaderInfo {
	uint32_t structType;
	uint32_t structVersion;
	size_t structSize;
	uint32_t minInterfaceVersion;
	uint32_t maxInterfaceVersion;
	XrVersion minApiVersion;
	XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
	uint32_t structType;
	uint32_t structVersion;
	size_t structSize;
	uint32_t runtimeInterfaceVersion;
	XrVersion runtimeApiVersion;
	PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo *loaderInfo,
    XrNegotiateRuntimeRequest *runtimeRequest);


// ============================================================================
// Standalone session state
// ============================================================================

typedef struct StandaloneState {
	void *runtime_lib;
	PFN_xrGetInstanceProcAddr gipa;
#if defined(_WIN32)
	// D3D12 path (when Unity uses Direct3D 12)
	ID3D12Device *d3d12_device;        // Our own device (for runtime session)
	ID3D12CommandQueue *d3d12_queue;
	ID3D12CommandAllocator *d3d12_cmd_alloc;
	ID3D12GraphicsCommandList *d3d12_cmd_list;
	ID3D12Fence *d3d12_fence;
	HANDLE d3d12_fence_event;
	UINT64 d3d12_fence_value;
	ID3D12Resource *d3d12_shared_texture;  // On our device
	HANDLE d3d12_shared_handle;            // DXGI shared handle (cross-device)
	ID3D12Resource *d3d12_unity_shared;    // Same texture opened on Unity's device
	// D3D12 atlas bridge: shared texture for cross-device atlas blit
	ID3D12Resource *d3d12_atlas_bridge;       // On our device, SHARED
	HANDLE d3d12_atlas_bridge_handle;         // DXGI shared handle
	ID3D12Resource *d3d12_unity_atlas_bridge; // Opened on Unity's device
	// D3D11 path (when Unity uses Direct3D 11)
	ID3D11Device *d3d11_device;             // Our own D3D11 device (for runtime session)
	ID3D11DeviceContext *d3d11_context;     // Immediate context for d3d11_device
	ID3D11Texture2D *d3d11_shared_texture; // Weaved output on our device, SHARED
	HANDLE d3d11_shared_handle;             // DXGI shared handle for output texture
	ID3D11Texture2D *d3d11_unity_shared;    // Output texture opened on Unity's device
	// D3D11 atlas bridge: shared texture for cross-device atlas blit
	ID3D11Texture2D *d3d11_atlas_bridge;       // On our device, SHARED
	HANDLE d3d11_atlas_bridge_handle;          // DXGI shared handle
	ID3D11Texture2D *d3d11_unity_atlas_bridge; // Opened on Unity's device
#endif

	XrInstance instance;
	XrSystemId system_id;
	XrSession session;
	XrSpace local_space;

	XrSessionState session_state;
	volatile int running;
	int session_ready;

	DisplayXRDisplayInfo display_info;

	float left_eye[3];
	float right_eye[3];
	int is_tracked;

	// Tunables + display pose (set from C#)
	Display3DTunables tunables;
	int tunables_set;
	int camera_centric;
	float inv_convergence_distance;
	float fov_override; // half_tan_vfov for camera-centric
	XrPosef display_pose;
	int display_pose_set;

	uint32_t tex_width;
	uint32_t tex_height;
	volatile int tex_ready;

	// Single atlas swapchain (all views tiled into one texture)
	SASwapchain atlas;
	int atlas_created;

	// Rendering mode metadata (from xrEnumerateDisplayRenderingModesEXT)
	XrDisplayRenderingModeInfoEXT rendering_modes[SA_MAX_RENDERING_MODES];
	uint32_t rendering_mode_count;
	uint32_t current_rendering_mode_index;

	// Multi-view eye positions (from xrLocateViews)
	float eye_positions[SA_MAX_VIEWS][3];
	uint32_t located_view_count;

	// Last computed Kooima views (from compute_views, used by submit_frame_atlas)
	Display3DView computed_views[SA_MAX_VIEWS];
	uint32_t computed_view_count;

	// Frame state (stored between begin_frame and submit/end)
	XrTime predicted_display_time;
	int frame_begun;

	PFN_xrDestroyInstance pfn_destroy_instance;
	PFN_xrGetSystem pfn_get_system;
	PFN_xrGetSystemProperties pfn_get_system_properties;
	PFN_xrCreateSession pfn_create_session;
	PFN_xrDestroySession pfn_destroy_session;
	PFN_xrCreateReferenceSpace pfn_create_reference_space;
	PFN_xrDestroySpace pfn_destroy_space;
	PFN_xrBeginSession pfn_begin_session;
	PFN_xrEndSession pfn_end_session;
	PFN_xrRequestExitSession pfn_request_exit_session;
	PFN_xrWaitFrame pfn_wait_frame;
	PFN_xrBeginFrame pfn_begin_frame;
	PFN_xrEndFrame pfn_end_frame;
	PFN_xrPollEvent pfn_poll_event;
	PFN_xrLocateViews pfn_locate_views;
	PFN_xrEnumerateSwapchainFormats pfn_enumerate_swapchain_formats;
	PFN_xrCreateSwapchain pfn_create_swapchain;
	PFN_xrDestroySwapchain pfn_destroy_swapchain;
	PFN_xrEnumerateSwapchainImages pfn_enumerate_swapchain_images;
	PFN_xrAcquireSwapchainImage pfn_acquire_swapchain_image;
	PFN_xrWaitSwapchainImage pfn_wait_swapchain_image;
	PFN_xrReleaseSwapchainImage pfn_release_swapchain_image;

	// Display mode extensions (optional — resolved after instance creation)
	PFN_xrRequestDisplayModeEXT pfn_request_display_mode;
	PFN_xrRequestDisplayRenderingModeEXT pfn_request_rendering_mode;
	PFN_xrEnumerateDisplayRenderingModesEXT pfn_enumerate_rendering_modes;
	PFN_xrSetSharedTextureOutputRectEXT pfn_set_output_rect;
	uint32_t canvas_width;
	uint32_t canvas_height;
	int32_t canvas_x;
	int32_t canvas_y;
	int has_display_mode_ext;
} StandaloneState;

static StandaloneState s_sa = {};

#if defined(_WIN32)
// Fullscreen window state — play mode output on the 3D display.
// HWND is created before session start (prepare_fullscreen_window) so it can
// be bound at xrCreateSession time for display-processor position tracking.
// The runtime writes weaved output to s_sa.d3d12_shared_texture (shared texture
// mode); present() blits that to our own DXGI swap chain on the HWND.
static HWND                     s_fw_hwnd;
static volatile int             s_fw_escape_pressed;
static IDXGISwapChain3         *s_fw_swapchain;
static ID3D12Resource          *s_fw_bb[2];       // swap chain back buffers
static ID3D12CommandAllocator  *s_fw_cmd_alloc;
static ID3D12GraphicsCommandList *s_fw_cmd_list;
static ID3D12Fence             *s_fw_fence;
static HANDLE                   s_fw_fence_event;
static uint64_t                 s_fw_fence_value;
static bool                     s_fw_is_fullscreen;
static RECT                     s_fw_windowed_rect; // saved windowed pos/size
static uint32_t                 s_fw_sc_w;          // current swap chain width
static uint32_t                 s_fw_sc_h;          // current swap chain height
// D3D11 fullscreen window back buffers (used when s_use_d3d11)
static ID3D11Texture2D         *s_fw_bb11[2];
#endif

// (displayxr_standalone_fullscreen_window_hide declared in displayxr_standalone.h)

// ============================================================================
// JSON parsing helper (minimal, no external deps)
// ============================================================================

static char *
parse_library_path(const char *json_path)
{
	FILE *f = fopen(json_path, "r");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = (char *)malloc(len + 1);
	if (!buf) { fclose(f); return NULL; }
	fread(buf, 1, len, f);
	buf[len] = '\0';
	fclose(f);

	const char *key = "\"library_path\"";
	char *pos = strstr(buf, key);
	if (!pos) { free(buf); return NULL; }
	pos += strlen(key);

	while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':') pos++;
	if (*pos != '"') { free(buf); return NULL; }
	pos++;

	char *end = strchr(pos, '"');
	if (!end) { free(buf); return NULL; }

	size_t path_len = end - pos;
	char *result = (char *)malloc(path_len + 1);
	memcpy(result, pos, path_len);
	result[path_len] = '\0';

	free(buf);
	return result;
}

static char *
resolve_library_path(const char *json_path, const char *lib_path)
{
	if (lib_path[0] == '/') {
		return strdup(lib_path);
	}

	const char *last_sep = strrchr(json_path, '/');
#if defined(_WIN32)
	const char *last_bsep = strrchr(json_path, '\\');
	if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;
#endif

	size_t dir_len = last_sep ? (size_t)(last_sep - json_path + 1) : 0;
	size_t lib_len = strlen(lib_path);

	char *result = (char *)malloc(dir_len + lib_len + 1);
	if (dir_len > 0) memcpy(result, json_path, dir_len);
	memcpy(result + dir_len, lib_path, lib_len);
	result[dir_len + lib_len] = '\0';

	return result;
}


// ============================================================================
// Resolve OpenXR function pointers
// ============================================================================

#define SA_RESOLVE_FN(xr_name, field_name, type) do { \
	PFN_xrVoidFunction fn = NULL; \
	if (XR_FAILED(s_sa.gipa(s_sa.instance, #xr_name, &fn)) || !fn) { \
		fprintf(stderr, "[DisplayXR-SA] Failed to resolve %s\n", #xr_name); \
		return 0; \
	} \
	s_sa.field_name = (type)fn; \
} while(0)

static int
resolve_functions(void)
{
	SA_RESOLVE_FN(xrDestroyInstance, pfn_destroy_instance, PFN_xrDestroyInstance);
	SA_RESOLVE_FN(xrGetSystem, pfn_get_system, PFN_xrGetSystem);
	SA_RESOLVE_FN(xrGetSystemProperties, pfn_get_system_properties, PFN_xrGetSystemProperties);
	SA_RESOLVE_FN(xrCreateSession, pfn_create_session, PFN_xrCreateSession);
	SA_RESOLVE_FN(xrDestroySession, pfn_destroy_session, PFN_xrDestroySession);
	SA_RESOLVE_FN(xrCreateReferenceSpace, pfn_create_reference_space, PFN_xrCreateReferenceSpace);
	SA_RESOLVE_FN(xrDestroySpace, pfn_destroy_space, PFN_xrDestroySpace);
	SA_RESOLVE_FN(xrBeginSession, pfn_begin_session, PFN_xrBeginSession);
	SA_RESOLVE_FN(xrEndSession, pfn_end_session, PFN_xrEndSession);
	SA_RESOLVE_FN(xrRequestExitSession, pfn_request_exit_session, PFN_xrRequestExitSession);
	SA_RESOLVE_FN(xrWaitFrame, pfn_wait_frame, PFN_xrWaitFrame);
	SA_RESOLVE_FN(xrBeginFrame, pfn_begin_frame, PFN_xrBeginFrame);
	SA_RESOLVE_FN(xrEndFrame, pfn_end_frame, PFN_xrEndFrame);
	SA_RESOLVE_FN(xrPollEvent, pfn_poll_event, PFN_xrPollEvent);
	SA_RESOLVE_FN(xrLocateViews, pfn_locate_views, PFN_xrLocateViews);
	SA_RESOLVE_FN(xrEnumerateSwapchainFormats, pfn_enumerate_swapchain_formats, PFN_xrEnumerateSwapchainFormats);
	SA_RESOLVE_FN(xrCreateSwapchain, pfn_create_swapchain, PFN_xrCreateSwapchain);
	SA_RESOLVE_FN(xrDestroySwapchain, pfn_destroy_swapchain, PFN_xrDestroySwapchain);
	SA_RESOLVE_FN(xrEnumerateSwapchainImages, pfn_enumerate_swapchain_images, PFN_xrEnumerateSwapchainImages);
	SA_RESOLVE_FN(xrAcquireSwapchainImage, pfn_acquire_swapchain_image, PFN_xrAcquireSwapchainImage);
	SA_RESOLVE_FN(xrWaitSwapchainImage, pfn_wait_swapchain_image, PFN_xrWaitSwapchainImage);
	SA_RESOLVE_FN(xrReleaseSwapchainImage, pfn_release_swapchain_image, PFN_xrReleaseSwapchainImage);

	// Optional display mode extensions (don't fail if missing)
	{
		PFN_xrVoidFunction fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrRequestDisplayModeEXT", &fn)) && fn) {
			s_sa.pfn_request_display_mode = (PFN_xrRequestDisplayModeEXT)fn;
			s_sa.has_display_mode_ext = 1;
			fprintf(stderr, "[DisplayXR-SA] Resolved xrRequestDisplayModeEXT\n");
		}
		fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrRequestDisplayRenderingModeEXT", &fn)) && fn) {
			s_sa.pfn_request_rendering_mode = (PFN_xrRequestDisplayRenderingModeEXT)fn;
			fprintf(stderr, "[DisplayXR-SA] Resolved xrRequestDisplayRenderingModeEXT\n");
		}
		fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrEnumerateDisplayRenderingModesEXT", &fn)) && fn) {
			s_sa.pfn_enumerate_rendering_modes = (PFN_xrEnumerateDisplayRenderingModesEXT)fn;
			fprintf(stderr, "[DisplayXR-SA] Resolved xrEnumerateDisplayRenderingModesEXT\n");
		}
		fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrSetSharedTextureOutputRectEXT", &fn)) && fn) {
			s_sa.pfn_set_output_rect = (PFN_xrSetSharedTextureOutputRectEXT)fn;
			fprintf(stderr, "[DisplayXR-SA] Resolved xrSetSharedTextureOutputRectEXT\n");
		}
	}

	return 1;
}


// ============================================================================
// Rendering mode helpers
// ============================================================================

static void
enumerate_and_store_modes(void)
{
	s_sa.rendering_mode_count = 0;
	if (!s_sa.pfn_enumerate_rendering_modes || s_sa.session == XR_NULL_HANDLE)
		return;

	uint32_t total = 0;
	XrResult result = s_sa.pfn_enumerate_rendering_modes(s_sa.session, 0, &total, NULL);
	if (XR_FAILED(result) || total == 0) return;

	if (total > SA_MAX_RENDERING_MODES) total = SA_MAX_RENDERING_MODES;
	for (uint32_t i = 0; i < total; i++) {
		s_sa.rendering_modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		s_sa.rendering_modes[i].next = NULL;
	}

	result = s_sa.pfn_enumerate_rendering_modes(s_sa.session, total, &total, s_sa.rendering_modes);
	if (XR_FAILED(result)) return;

	s_sa.rendering_mode_count = total;
	for (uint32_t i = 0; i < total; i++) {
		fprintf(stderr, "[DisplayXR-SA] Mode[%u]: '%s' views=%u tiles=%ux%u viewPx=%ux%u scale=%.2fx%.2f hw3d=%d\n",
		        s_sa.rendering_modes[i].modeIndex,
		        s_sa.rendering_modes[i].modeName,
		        s_sa.rendering_modes[i].viewCount,
		        s_sa.rendering_modes[i].tileColumns,
		        s_sa.rendering_modes[i].tileRows,
		        s_sa.rendering_modes[i].viewWidthPixels,
		        s_sa.rendering_modes[i].viewHeightPixels,
		        (double)s_sa.rendering_modes[i].viewScaleX,
		        (double)s_sa.rendering_modes[i].viewScaleY,
		        (int)s_sa.rendering_modes[i].hardwareDisplay3D);
	}
}

/// Get the current rendering mode info. Returns NULL if no modes enumerated.
static const XrDisplayRenderingModeInfoEXT *
get_current_mode(void)
{
	for (uint32_t i = 0; i < s_sa.rendering_mode_count; i++) {
		if (s_sa.rendering_modes[i].modeIndex == s_sa.current_rendering_mode_index)
			return &s_sa.rendering_modes[i];
	}
	return (s_sa.rendering_mode_count > 0) ? &s_sa.rendering_modes[0] : NULL;
}

/// Get tiling parameters for a rendering mode (or defaults if NULL).
static void
get_mode_tiling(const XrDisplayRenderingModeInfoEXT *mode,
                uint32_t *view_count, uint32_t *tile_cols, uint32_t *tile_rows,
                uint32_t *view_w, uint32_t *view_h)
{
	if (mode && mode->tileColumns > 0 && mode->tileRows > 0) {
		*view_count = mode->viewCount > 0 ? mode->viewCount : 2;
		*tile_cols = mode->tileColumns;
		*tile_rows = mode->tileRows;
		*view_w = mode->viewWidthPixels > 0
			? mode->viewWidthPixels
			: (uint32_t)(mode->viewScaleX * s_sa.display_info.display_pixel_width);
		*view_h = mode->viewHeightPixels > 0
			? mode->viewHeightPixels
			: (uint32_t)(mode->viewScaleY * s_sa.display_info.display_pixel_height);
	} else {
		// Fallback: stereo SBS
		*view_count = 2;
		*tile_cols = 2;
		*tile_rows = 1;
		*view_w = s_sa.display_info.display_pixel_width;
		*view_h = s_sa.display_info.display_pixel_height;
	}
}


// Canvas-aware render tiling: computes per-view render dimensions from canvas
// dims (matching the reference app pattern). Falls back to display-based dims
// if canvas is not set. Used for actual rendering; get_mode_tiling (above)
// is used for worst-case atlas/swapchain sizing.
static void
get_render_tiling(const XrDisplayRenderingModeInfoEXT *mode,
                  uint32_t *view_count, uint32_t *tile_cols, uint32_t *tile_rows,
                  uint32_t *view_w, uint32_t *view_h)
{
	if (!mode || mode->tileColumns == 0 || mode->tileRows == 0) {
		// Fallback: use display-based tiling
		get_mode_tiling(mode, view_count, tile_cols, tile_rows, view_w, view_h);
		return;
	}

	*view_count = mode->viewCount > 0 ? mode->viewCount : 2;
	*tile_cols = mode->tileColumns;
	*tile_rows = mode->tileRows;

	if (s_sa.canvas_width > 0 && s_sa.canvas_height > 0) {
		int display3D = (int)mode->hardwareDisplay3D;
		if (!display3D) {
			// 2D mode: render at canvas size
			*view_w = s_sa.canvas_width;
			*view_h = s_sa.canvas_height;
		} else {
			// 3D mode: canvas × view scale
			float sx = mode->viewScaleX > 0 ? mode->viewScaleX : 0.5f;
			float sy = mode->viewScaleY > 0 ? mode->viewScaleY : 0.5f;
			*view_w = (uint32_t)(s_sa.canvas_width * sx);
			*view_h = (uint32_t)(s_sa.canvas_height * sy);
		}

		// Clamp to atlas tile limits
		uint32_t max_vw, max_vh, dummy_vc, dummy_tc, dummy_tr;
		get_mode_tiling(mode, &dummy_vc, &dummy_tc, &dummy_tr, &max_vw, &max_vh);
		if (*view_w > max_vw) *view_w = max_vw;
		if (*view_h > max_vh) *view_h = max_vh;
	} else {
		// No canvas set — fall back to display-based dims
		get_mode_tiling(mode, view_count, tile_cols, tile_rows, view_w, view_h);
	}
}


#if defined(_WIN32)
// Forward declarations — defined in the public API section below.
static ID3D12Device *s_unity_d3d12_device;
static ID3D11Device *s_unity_d3d11_device;
static int           s_use_d3d11; // 0 = D3D12 (default), 1 = D3D11
#endif

// ============================================================================
// Swapchain management (single atlas)
// ============================================================================

static int
create_atlas_swapchain(void)
{
	if (s_sa.atlas_created) return 1;
	if (!s_sa.display_info.is_valid) return 0;

	// Compute max atlas size across all rendering modes
	uint32_t atlas_w = 0, atlas_h = 0;

	if (s_sa.rendering_mode_count > 0) {
		for (uint32_t i = 0; i < s_sa.rendering_mode_count; i++) {
			uint32_t vc, tc, tr, vw, vh;
			get_mode_tiling(&s_sa.rendering_modes[i], &vc, &tc, &tr, &vw, &vh);
			uint32_t mw = tc * vw;
			uint32_t mh = tr * vh;
			if (mw > atlas_w) atlas_w = mw;
			if (mh > atlas_h) atlas_h = mh;
		}
	}

	// Fallback: stereo SBS at display resolution
	if (atlas_w == 0 || atlas_h == 0) {
		atlas_w = s_sa.display_info.display_pixel_width * 2;
		atlas_h = s_sa.display_info.display_pixel_height;
	}

	// Enumerate supported formats and pick one
	uint32_t fmt_count = 0;
	s_sa.pfn_enumerate_swapchain_formats(s_sa.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) {
		fprintf(stderr, "[DisplayXR-SA] No swapchain formats available\n");
		return 0;
	}

	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_sa.pfn_enumerate_swapchain_formats(s_sa.session, fmt_count, &fmt_count, formats);

	// Pick a suitable format per platform
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) {
		fprintf(stderr, "[DisplayXR-SA] Supported format[%u]: %lld\n", i, formats[i]);
#if defined(__APPLE__)
		if (formats[i] == 80) { format = 80; break; } // MTLPixelFormatBGRA8Unorm
		if (formats[i] == 70) { format = 70; }         // MTLPixelFormatRGBA8Unorm
#elif defined(_WIN32)
		if (formats[i] == 28) { format = 28; break; }  // DXGI_FORMAT_R8G8B8A8_UNORM
		if (formats[i] == 87) { format = 87; }          // DXGI_FORMAT_B8G8R8A8_UNORM
#endif
	}
	fprintf(stderr, "[DisplayXR-SA] Selected swapchain format: %lld\n", format);

	XrSwapchainCreateInfo sc_ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	sc_ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	sc_ci.format = format;
	sc_ci.sampleCount = 1;
	sc_ci.width = atlas_w;
	sc_ci.height = atlas_h;
	sc_ci.faceCount = 1;
	sc_ci.arraySize = 1;
	sc_ci.mipCount = 1;

	XrResult result = s_sa.pfn_create_swapchain(
		s_sa.session, &sc_ci, &s_sa.atlas.handle);
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrCreateSwapchain (atlas) failed: %d\n", result);
		return 0;
	}

	s_sa.atlas.width = atlas_w;
	s_sa.atlas.height = atlas_h;
	s_sa.atlas.format = format;

	// Enumerate swapchain images
	uint32_t count = 0;
	s_sa.pfn_enumerate_swapchain_images(s_sa.atlas.handle, 0, &count, NULL);
	if (count > SA_MAX_SWAPCHAIN_IMAGES) count = SA_MAX_SWAPCHAIN_IMAGES;
	s_sa.atlas.image_count = count;

	for (uint32_t i = 0; i < count; i++) {
#if defined(__APPLE__)
		s_sa.atlas.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
		s_sa.atlas.images[i].next = NULL;
#elif defined(_WIN32)
		if (s_use_d3d11) {
			s_sa.atlas.images_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
			s_sa.atlas.images_d3d11[i].next = NULL;
		} else {
			s_sa.atlas.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
			s_sa.atlas.images[i].next = NULL;
		}
#endif
	}

#if defined(_WIN32)
	result = s_sa.pfn_enumerate_swapchain_images(
		s_sa.atlas.handle, count, &count,
		s_use_d3d11
			? (XrSwapchainImageBaseHeader *)s_sa.atlas.images_d3d11
			: (XrSwapchainImageBaseHeader *)s_sa.atlas.images);
#else
	result = s_sa.pfn_enumerate_swapchain_images(
		s_sa.atlas.handle, count, &count,
		(XrSwapchainImageBaseHeader *)s_sa.atlas.images);
#endif
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrEnumerateSwapchainImages (atlas) failed: %d\n", result);
		return 0;
	}

	fprintf(stderr, "[DisplayXR-SA] Atlas swapchain: %ux%u, %u images\n",
	        atlas_w, atlas_h, count);

	s_sa.atlas_created = 1;

#if defined(_WIN32)
	// Create atlas bridge shared texture (same dimensions as swapchain atlas).
	// This bridges Unity's device and our device for cross-device atlas blit.
	if (s_use_d3d11) {
		// D3D11 atlas bridge
		if (s_unity_d3d11_device && s_sa.d3d11_device) {
			D3D11_TEXTURE2D_DESC bd = {};
			bd.Width  = atlas_w;
			bd.Height = atlas_h;
			bd.MipLevels = 1;
			bd.ArraySize = 1;
			bd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			bd.SampleDesc.Count = 1;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			bd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

			HRESULT hr = s_sa.d3d11_device->CreateTexture2D(
				&bd, NULL, &s_sa.d3d11_atlas_bridge);
			if (FAILED(hr)) {
				fprintf(stderr, "[DisplayXR-SA] D3D11 atlas bridge CreateTexture2D failed: 0x%08lx\n", hr);
			} else {
				IDXGIResource *dxgi_res = NULL;
				hr = s_sa.d3d11_atlas_bridge->QueryInterface(
					__uuidof(IDXGIResource), (void **)&dxgi_res);
				if (SUCCEEDED(hr) && dxgi_res) {
					dxgi_res->GetSharedHandle(&s_sa.d3d11_atlas_bridge_handle);
					dxgi_res->Release();
				}
				if (s_sa.d3d11_atlas_bridge_handle) {
					hr = s_unity_d3d11_device->OpenSharedResource(
						s_sa.d3d11_atlas_bridge_handle,
						__uuidof(ID3D11Texture2D),
						(void **)&s_sa.d3d11_unity_atlas_bridge);
					if (SUCCEEDED(hr) && s_sa.d3d11_unity_atlas_bridge) {
						fprintf(stderr, "[DisplayXR-SA] D3D11 atlas bridge: %ux%u, handle=%p, unity_res=%p\n",
						        atlas_w, atlas_h, s_sa.d3d11_atlas_bridge_handle,
						        (void *)s_sa.d3d11_unity_atlas_bridge);
					} else {
						fprintf(stderr, "[DisplayXR-SA] D3D11 atlas bridge OpenSharedResource failed: 0x%08lx\n", hr);
					}
				}
			}
		}
	} else {
		// D3D12 atlas bridge
		if (s_unity_d3d12_device && s_sa.d3d12_device) {
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			bd.Width = atlas_w;
			bd.Height = atlas_h;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			HRESULT hr = s_sa.d3d12_device->CreateCommittedResource(
				&heap, D3D12_HEAP_FLAG_SHARED, &bd,
				D3D12_RESOURCE_STATE_COMMON, NULL,
				__uuidof(ID3D12Resource), (void **)&s_sa.d3d12_atlas_bridge);
			if (FAILED(hr)) {
				fprintf(stderr, "[DisplayXR-SA] Atlas bridge CreateCommittedResource failed: 0x%08lx\n", hr);
			} else {
				hr = s_sa.d3d12_device->CreateSharedHandle(
					s_sa.d3d12_atlas_bridge, NULL, GENERIC_ALL, NULL,
					&s_sa.d3d12_atlas_bridge_handle);
				if (SUCCEEDED(hr) && s_sa.d3d12_atlas_bridge_handle) {
					hr = s_unity_d3d12_device->OpenSharedHandle(
						s_sa.d3d12_atlas_bridge_handle,
						__uuidof(ID3D12Resource), (void **)&s_sa.d3d12_unity_atlas_bridge);
					if (SUCCEEDED(hr) && s_sa.d3d12_unity_atlas_bridge) {
						fprintf(stderr, "[DisplayXR-SA] Atlas bridge: %ux%u, handle=%p, unity_res=%p\n",
						        atlas_w, atlas_h, s_sa.d3d12_atlas_bridge_handle,
						        (void *)s_sa.d3d12_unity_atlas_bridge);
					} else {
						fprintf(stderr, "[DisplayXR-SA] Atlas bridge OpenSharedHandle failed: 0x%08lx\n", hr);
					}
				}
			}
		}
	}
#endif

	return 1;
}

static void
destroy_atlas_swapchain(void)
{
	if (s_sa.atlas.handle != XR_NULL_HANDLE && s_sa.pfn_destroy_swapchain) {
		s_sa.pfn_destroy_swapchain(s_sa.atlas.handle);
		s_sa.atlas.handle = XR_NULL_HANDLE;
	}
	s_sa.atlas_created = 0;
}


// ============================================================================
// Public API: Session lifecycle
// ============================================================================

#if defined(_WIN32)
// Unity's D3D12 device — forward-declared above, initialized here.
// Extracted from a native texture pointer via GetDevice().
// Set by C# before calling displayxr_standalone_start().
// (declaration is above create_atlas_swapchain which uses it)
#endif

void
displayxr_standalone_set_unity_device(void *unity_native_tex)
{
#if defined(_WIN32)
	if (!unity_native_tex) {
		fprintf(stderr, "[DisplayXR-SA] set_unity_device: null texture\n");
		return;
	}
	fprintf(stderr, "[DisplayXR-SA] set_unity_device: native tex ptr=%p\n", unity_native_tex);

	IUnknown *unk = (IUnknown *)unity_native_tex;

	// Try D3D12 first
	ID3D12Resource *res12 = NULL;
	HRESULT hr = unk->QueryInterface(__uuidof(ID3D12Resource), (void **)&res12);
	if (SUCCEEDED(hr) && res12) {
		ID3D12Device *dev = NULL;
		hr = res12->GetDevice(__uuidof(ID3D12Device), (void **)&dev);
		res12->Release();
		if (SUCCEEDED(hr) && dev) {
			s_unity_d3d12_device = dev;
			s_use_d3d11 = 0;
			fprintf(stderr, "[DisplayXR-SA] Using Unity's D3D12 device: %p\n", (void *)dev);
		} else {
			fprintf(stderr, "[DisplayXR-SA] GetDevice (D3D12) failed: hr=0x%08lx\n", hr);
		}
		return;
	}

	// Try D3D11
	ID3D11Resource *res11 = NULL;
	hr = unk->QueryInterface(__uuidof(ID3D11Resource), (void **)&res11);
	if (SUCCEEDED(hr) && res11) {
		ID3D11Device *dev = NULL;
		res11->GetDevice(&dev);
		res11->Release();
		if (dev) {
			s_unity_d3d11_device = dev;
			s_use_d3d11 = 1;
			fprintf(stderr, "[DisplayXR-SA] Using Unity's D3D11 device: %p\n", (void *)dev);
		} else {
			fprintf(stderr, "[DisplayXR-SA] GetDevice (D3D11) failed\n");
		}
		return;
	}

	fprintf(stderr, "[DisplayXR-SA] Texture is neither D3D12 nor D3D11 resource (hr=0x%08lx)\n", hr);
#else
	(void)unity_native_tex;
#endif
}

int
displayxr_standalone_start(const char *runtime_json_path)
{
	if (s_sa.running) {
		fprintf(stderr, "[DisplayXR-SA] Already running\n");
		return 1;
	}

	memset(&s_sa, 0, sizeof(s_sa));

#if defined(_WIN32)
	// Set Per-Monitor DPI Awareness V2 so the Leia SR weaver sees physical
	// pixels from GetClientRect(), matching our canvas rect dimensions.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

	fprintf(stderr, "[DisplayXR-SA] Starting with runtime: %s\n", runtime_json_path);

	// --- Step 1: Load the runtime library ---
	char *lib_rel = parse_library_path(runtime_json_path);
	if (!lib_rel) {
		fprintf(stderr, "[DisplayXR-SA] Failed to parse library_path from %s\n", runtime_json_path);
		return 0;
	}

	char *lib_abs = resolve_library_path(runtime_json_path, lib_rel);
	free(lib_rel);

	fprintf(stderr, "[DisplayXR-SA] Loading runtime library: %s\n", lib_abs);

	XrResult result;

#if defined(_WIN32)
	HMODULE hmod = LoadLibraryA(lib_abs);
	if (!hmod) {
		fprintf(stderr, "[DisplayXR-SA] LoadLibrary failed: %lu\n", GetLastError());
		free(lib_abs);
		return 0;
	}
	s_sa.runtime_lib = (void *)hmod;
	free(lib_abs);

	// --- Step 2: Negotiate with the runtime ---
	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
		(PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(hmod,
		    "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) {
		fprintf(stderr, "[DisplayXR-SA] Runtime doesn't export xrNegotiateLoaderRuntimeInterface\n");
		FreeLibrary(hmod);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	XrNegotiateLoaderInfo loader_info = {};
	loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loader_info.structVersion = 1;
	loader_info.structSize = sizeof(XrNegotiateLoaderInfo);
	loader_info.minInterfaceVersion = 1;
	loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	loader_info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

	XrNegotiateRuntimeRequest runtime_req = {};
	runtime_req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	runtime_req.structVersion = 1;
	runtime_req.structSize = sizeof(XrNegotiateRuntimeRequest);

	result = negotiate(&loader_info, &runtime_req);
	if (XR_FAILED(result) || !runtime_req.getInstanceProcAddr) {
		fprintf(stderr, "[DisplayXR-SA] Runtime negotiation failed: %d\n", result);
		FreeLibrary(hmod);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	s_sa.gipa = runtime_req.getInstanceProcAddr;
	fprintf(stderr, "[DisplayXR-SA] Runtime negotiation succeeded\n");
#else
	s_sa.runtime_lib = dlopen(lib_abs, RTLD_LOCAL | RTLD_LAZY);
	if (!s_sa.runtime_lib) {
		fprintf(stderr, "[DisplayXR-SA] dlopen failed: %s\n", dlerror());
		free(lib_abs);
		return 0;
	}
	free(lib_abs);

	// --- Step 2: Negotiate with the runtime ---
	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
		(PFN_xrNegotiateLoaderRuntimeInterface)dlsym(s_sa.runtime_lib,
		    "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) {
		fprintf(stderr, "[DisplayXR-SA] Runtime doesn't export xrNegotiateLoaderRuntimeInterface\n");
		dlclose(s_sa.runtime_lib);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	XrNegotiateLoaderInfo loader_info = {};
	loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loader_info.structVersion = 1;
	loader_info.structSize = sizeof(XrNegotiateLoaderInfo);
	loader_info.minInterfaceVersion = 1;
	loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	loader_info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

	XrNegotiateRuntimeRequest runtime_req = {};
	runtime_req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	runtime_req.structVersion = 1;
	runtime_req.structSize = sizeof(XrNegotiateRuntimeRequest);

	result = negotiate(&loader_info, &runtime_req);
	if (XR_FAILED(result) || !runtime_req.getInstanceProcAddr) {
		fprintf(stderr, "[DisplayXR-SA] Runtime negotiation failed: %d\n", result);
		dlclose(s_sa.runtime_lib);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	s_sa.gipa = runtime_req.getInstanceProcAddr;
	fprintf(stderr, "[DisplayXR-SA] Runtime negotiation succeeded\n");
#endif

	// --- Step 3: Create OpenXR instance ---
#if defined(__APPLE__)
	const char *extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
		XR_KHR_METAL_ENABLE_EXTENSION_NAME,
		XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME,
	};
	uint32_t ext_count = sizeof(extensions) / sizeof(extensions[0]);
#elif defined(_WIN32)
	// Choose D3D extension at runtime based on which Unity graphics API is active.
	const char *d3d_ext = s_use_d3d11 ? "XR_KHR_D3D11_enable" : "XR_KHR_D3D12_enable";
	const char *extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
		d3d_ext,
		XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME,
	};
	uint32_t ext_count = sizeof(extensions) / sizeof(extensions[0]);
#else
	const char *extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
	};
	uint32_t ext_count = sizeof(extensions) / sizeof(extensions[0]);
#endif

	PFN_xrVoidFunction fn_create = NULL;
	s_sa.gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn_create);
	if (!fn_create) {
		fprintf(stderr, "[DisplayXR-SA] Failed to resolve xrCreateInstance\n");
		displayxr_standalone_stop();
		return 0;
	}

	XrInstanceCreateInfo instance_ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(instance_ci.applicationInfo.applicationName, "DisplayXR Preview", XR_MAX_APPLICATION_NAME_SIZE);
	instance_ci.applicationInfo.applicationVersion = 1;
	strncpy(instance_ci.applicationInfo.engineName, "Unity", XR_MAX_ENGINE_NAME_SIZE);
	instance_ci.applicationInfo.engineVersion = 1;
	instance_ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	instance_ci.enabledExtensionCount = ext_count;
	instance_ci.enabledExtensionNames = extensions;

	result = ((PFN_xrCreateInstance)fn_create)(&instance_ci, &s_sa.instance);
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrCreateInstance failed: %d\n", result);
		displayxr_standalone_stop();
		return 0;
	}
	fprintf(stderr, "[DisplayXR-SA] Instance created\n");

	// --- Step 4: Resolve all function pointers ---
	if (!resolve_functions()) {
		displayxr_standalone_stop();
		return 0;
	}

	// --- Step 5: Get system ---
	XrSystemGetInfo sys_info = {XR_TYPE_SYSTEM_GET_INFO};
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	result = s_sa.pfn_get_system(s_sa.instance, &sys_info, &s_sa.system_id);
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrGetSystem failed: %d\n", result);
		displayxr_standalone_stop();
		return 0;
	}
	fprintf(stderr, "[DisplayXR-SA] System acquired\n");

	// --- Step 5b: Get Metal graphics requirements ---
#if defined(__APPLE__)
	{
		PFN_xrVoidFunction fn_req = NULL;
		s_sa.gipa(s_sa.instance, "xrGetMetalGraphicsRequirementsKHR", &fn_req);
		if (fn_req) {
			XrGraphicsRequirementsMetalKHR req = {};
			req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
			result = ((PFN_xrGetMetalGraphicsRequirementsKHR)fn_req)(
				s_sa.instance, s_sa.system_id, &req);
			if (XR_FAILED(result)) {
				fprintf(stderr, "[DisplayXR-SA] xrGetMetalGraphicsRequirementsKHR failed: %d\n", result);
				displayxr_standalone_stop();
				return 0;
			}
			fprintf(stderr, "[DisplayXR-SA] Metal graphics requirements satisfied\n");
		} else {
			fprintf(stderr, "[DisplayXR-SA] Warning: xrGetMetalGraphicsRequirementsKHR not found\n");
		}
	}
#elif defined(_WIN32)
	// --- Step 5b: Graphics requirements + create device ---
	if (s_use_d3d11) {
		// D3D11 path: query requirements, create our own D3D11 device
		PFN_xrVoidFunction fn_req = NULL;
		s_sa.gipa(s_sa.instance, "xrGetD3D11GraphicsRequirementsKHR", &fn_req);

		LUID adapter_luid = {};
		if (fn_req) {
			XrGraphicsRequirementsD3D11KHR req = {};
			req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
			result = ((XrResult(XRAPI_CALL *)(XrInstance, XrSystemId, void *))fn_req)(
				s_sa.instance, s_sa.system_id, &req);
			if (XR_FAILED(result)) {
				fprintf(stderr, "[DisplayXR-SA] xrGetD3D11GraphicsRequirementsKHR failed: %d\n", result);
				displayxr_standalone_stop();
				return 0;
			}
			adapter_luid = req.adapterLuid;
			fprintf(stderr, "[DisplayXR-SA] D3D11 requirements: adapter LUID=%08lx-%08lx, minFeatureLevel=0x%x\n",
			        adapter_luid.HighPart, adapter_luid.LowPart, (unsigned)req.minFeatureLevel);
		}

		// Create our own D3D11 device for the runtime session (same reason as D3D12 —
		// sharing Unity's device causes conflicts with the compositor).
		{
			IDXGIFactory4 *factory = NULL;
			CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
			IDXGIAdapter1 *adapter = NULL;
			if (factory && (adapter_luid.HighPart != 0 || adapter_luid.LowPart != 0)) {
				HRESULT hr2 = factory->EnumAdapterByLuid(adapter_luid, __uuidof(IDXGIAdapter1), (void **)&adapter);
				if (SUCCEEDED(hr2)) {
					DXGI_ADAPTER_DESC1 desc;
					adapter->GetDesc1(&desc);
					fprintf(stderr, "[DisplayXR-SA] Found matching adapter: %ls\n", desc.Description);
				}
			}
			if (factory) factory->Release();

			UINT flags = 0;
			D3D_FEATURE_LEVEL feat_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			D3D_FEATURE_LEVEL chosen_level = D3D_FEATURE_LEVEL_11_0;
			HRESULT hr = D3D11CreateDevice(
				adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
				NULL, flags,
				feat_levels, 2,
				D3D11_SDK_VERSION,
				&s_sa.d3d11_device, &chosen_level, &s_sa.d3d11_context);
			if (adapter) adapter->Release();
			if (FAILED(hr) || !s_sa.d3d11_device) {
				fprintf(stderr, "[DisplayXR-SA] D3D11CreateDevice failed: 0x%08lx\n", hr);
				displayxr_standalone_stop();
				return 0;
			}
			fprintf(stderr, "[DisplayXR-SA] Created own D3D11 device for runtime session (featureLevel=0x%x)\n",
			        (unsigned)chosen_level);
		}
	} else {
		// D3D12 path: query requirements, create our own D3D12 device
		PFN_xrVoidFunction fn_req = NULL;
		s_sa.gipa(s_sa.instance, "xrGetD3D12GraphicsRequirementsKHR", &fn_req);

		LUID adapter_luid = {};
		if (fn_req) {
			XrGraphicsRequirementsD3D12KHR req = {};
			req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
			result = ((XrResult(XRAPI_CALL *)(XrInstance, XrSystemId, void *))fn_req)(
				s_sa.instance, s_sa.system_id, &req);
			if (XR_FAILED(result)) {
				fprintf(stderr, "[DisplayXR-SA] xrGetD3D12GraphicsRequirementsKHR failed: %d\n", result);
				displayxr_standalone_stop();
				return 0;
			}
			adapter_luid = req.adapterLuid;
			fprintf(stderr, "[DisplayXR-SA] D3D12 requirements: adapter LUID=%08lx-%08lx, minFeatureLevel=0x%x\n",
			        adapter_luid.HighPart, adapter_luid.LowPart, (unsigned)req.minFeatureLevel);
		}

		// Always create our own D3D12 device for the runtime session.
		// Sharing Unity's device caused device removal — the runtime's compositor
		// and Unity's renderer conflict when operating on the same device.
		// Cross-device texture sharing is done via DXGI shared handles.
		{
			IDXGIFactory4 *factory = NULL;
			CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
			IDXGIAdapter1 *adapter = NULL;
			if (factory && (adapter_luid.HighPart != 0 || adapter_luid.LowPart != 0)) {
				HRESULT hr2 = factory->EnumAdapterByLuid(adapter_luid, __uuidof(IDXGIAdapter1), (void **)&adapter);
				if (SUCCEEDED(hr2)) {
					DXGI_ADAPTER_DESC1 desc;
					adapter->GetDesc1(&desc);
					fprintf(stderr, "[DisplayXR-SA] Found matching adapter: %ls\n", desc.Description);
				}
			}
			HRESULT hr = D3D12CreateDevice(
				adapter, D3D_FEATURE_LEVEL_11_0,
				__uuidof(ID3D12Device), (void **)&s_sa.d3d12_device);
			if (adapter) adapter->Release();
			if (factory) factory->Release();
			if (FAILED(hr) || !s_sa.d3d12_device) {
				fprintf(stderr, "[DisplayXR-SA] D3D12CreateDevice failed: 0x%08lx\n", hr);
				displayxr_standalone_stop();
				return 0;
			}
			fprintf(stderr, "[DisplayXR-SA] Created own D3D12 device for runtime session\n");
		}

		// Create command queue on the device
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		HRESULT hr = s_sa.d3d12_device->CreateCommandQueue(
			&qd, __uuidof(ID3D12CommandQueue), (void **)&s_sa.d3d12_queue);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] CreateCommandQueue failed: 0x%08lx\n", hr);
			displayxr_standalone_stop();
			return 0;
		}

		// Create command allocator + command list for atlas blit
		s_sa.d3d12_device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(ID3D12CommandAllocator), (void **)&s_sa.d3d12_cmd_alloc);
		s_sa.d3d12_device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_sa.d3d12_cmd_alloc, NULL,
			__uuidof(ID3D12GraphicsCommandList), (void **)&s_sa.d3d12_cmd_list);
		s_sa.d3d12_cmd_list->Close(); // Start closed

		// Create fence for GPU sync
		s_sa.d3d12_device->CreateFence(
			0, D3D12_FENCE_FLAG_NONE,
			__uuidof(ID3D12Fence), (void **)&s_sa.d3d12_fence);
		s_sa.d3d12_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		s_sa.d3d12_fence_value = 0;

		fprintf(stderr, "[DisplayXR-SA] D3D12 command queue + blit resources created\n");
	}
#endif

	// --- Step 6: Get system properties + display info ---
	XrDisplayInfoEXT display_info_ext = {};
	display_info_ext.type = XR_TYPE_DISPLAY_INFO_EXT;

	XrSystemProperties sys_props = {XR_TYPE_SYSTEM_PROPERTIES};
	sys_props.next = &display_info_ext;

	result = s_sa.pfn_get_system_properties(s_sa.instance, s_sa.system_id, &sys_props);
	if (XR_SUCCEEDED(result)) {
		s_sa.display_info.display_width_meters = display_info_ext.displaySizeMeters.width;
		s_sa.display_info.display_height_meters = display_info_ext.displaySizeMeters.height;
		s_sa.display_info.display_pixel_width = display_info_ext.displayPixelWidth;
		s_sa.display_info.display_pixel_height = display_info_ext.displayPixelHeight;
		s_sa.display_info.nominal_viewer_x = display_info_ext.nominalViewerPositionInDisplaySpace.x;
		s_sa.display_info.nominal_viewer_y = display_info_ext.nominalViewerPositionInDisplaySpace.y;
		s_sa.display_info.nominal_viewer_z = display_info_ext.nominalViewerPositionInDisplaySpace.z;
		s_sa.display_info.recommended_view_scale_x = display_info_ext.recommendedViewScaleX;
		s_sa.display_info.recommended_view_scale_y = display_info_ext.recommendedViewScaleY;
		s_sa.display_info.is_valid = 1;

		fprintf(stderr, "[DisplayXR-SA] Display: %ux%u, %.3fx%.3fm\n",
		        display_info_ext.displayPixelWidth, display_info_ext.displayPixelHeight,
		        display_info_ext.displaySizeMeters.width, display_info_ext.displaySizeMeters.height);
	}

	// --- Step 7: Create shared texture ---
#if defined(__APPLE__)
	if (s_sa.display_info.is_valid &&
	    s_sa.display_info.display_pixel_width > 0 &&
	    s_sa.display_info.display_pixel_height > 0) {
		if (!displayxr_sa_metal_create(s_sa.display_info.display_pixel_width,
		                                s_sa.display_info.display_pixel_height)) {
			fprintf(stderr, "[DisplayXR-SA] IOSurface creation failed\n");
			displayxr_standalone_stop();
			return 0;
		}
		s_sa.tex_width = s_sa.display_info.display_pixel_width;
		s_sa.tex_height = s_sa.display_info.display_pixel_height;
		s_sa.tex_ready = 1;
	}
#elif defined(_WIN32)
	// --- Step 7: Create shared output texture ---
	if (s_sa.display_info.is_valid &&
	    s_sa.display_info.display_pixel_width > 0 &&
	    s_sa.display_info.display_pixel_height > 0)
	{
		uint32_t disp_w = s_sa.display_info.display_pixel_width;
		uint32_t disp_h = s_sa.display_info.display_pixel_height;

		if (s_use_d3d11) {
			// D3D11 shared texture
			D3D11_TEXTURE2D_DESC td = {};
			td.Width  = disp_w;
			td.Height = disp_h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

			HRESULT hr = s_sa.d3d11_device->CreateTexture2D(
				&td, NULL, &s_sa.d3d11_shared_texture);
			if (FAILED(hr)) {
				fprintf(stderr, "[DisplayXR-SA] D3D11 CreateTexture2D (shared) failed: 0x%08lx\n", hr);
				displayxr_standalone_stop();
				return 0;
			}

			IDXGIResource *dxgi_res = NULL;
			hr = s_sa.d3d11_shared_texture->QueryInterface(
				__uuidof(IDXGIResource), (void **)&dxgi_res);
			if (SUCCEEDED(hr) && dxgi_res) {
				dxgi_res->GetSharedHandle(&s_sa.d3d11_shared_handle);
				dxgi_res->Release();
			}
			if (!s_sa.d3d11_shared_handle) {
				fprintf(stderr, "[DisplayXR-SA] D3D11 GetSharedHandle failed\n");
				displayxr_standalone_stop();
				return 0;
			}

			s_sa.tex_width  = disp_w;
			s_sa.tex_height = disp_h;
			fprintf(stderr, "[DisplayXR-SA] D3D11 shared texture: %ux%u, handle=%p\n",
			        disp_w, disp_h, s_sa.d3d11_shared_handle);

			// Open on Unity's device for C# CreateExternalTexture
			if (s_unity_d3d11_device) {
				hr = s_unity_d3d11_device->OpenSharedResource(
					s_sa.d3d11_shared_handle,
					__uuidof(ID3D11Texture2D),
					(void **)&s_sa.d3d11_unity_shared);
				if (SUCCEEDED(hr) && s_sa.d3d11_unity_shared) {
					fprintf(stderr, "[DisplayXR-SA] D3D11 shared texture opened on Unity device: %p\n",
					        (void *)s_sa.d3d11_unity_shared);
				} else {
					fprintf(stderr, "[DisplayXR-SA] D3D11 OpenSharedResource on Unity device failed: 0x%08lx\n", hr);
				}
			}
			s_sa.tex_ready = 1;
		} else {
			// D3D12 shared texture
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width  = disp_w;
			td.Height = disp_h;
			td.DepthOrArraySize = 1;
			td.MipLevels = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			HRESULT hr = s_sa.d3d12_device->CreateCommittedResource(
				&heap, D3D12_HEAP_FLAG_SHARED, &td,
				D3D12_RESOURCE_STATE_COMMON, NULL,
				__uuidof(ID3D12Resource), (void **)&s_sa.d3d12_shared_texture);
			if (FAILED(hr)) {
				fprintf(stderr, "[DisplayXR-SA] CreateCommittedResource (shared) failed: 0x%08lx\n", hr);
				displayxr_standalone_stop();
				return 0;
			}

			hr = s_sa.d3d12_device->CreateSharedHandle(
				s_sa.d3d12_shared_texture, NULL, GENERIC_ALL, NULL,
				&s_sa.d3d12_shared_handle);
			if (FAILED(hr)) {
				fprintf(stderr, "[DisplayXR-SA] CreateSharedHandle failed: 0x%08lx\n", hr);
				displayxr_standalone_stop();
				return 0;
			}

			s_sa.tex_width  = (uint32_t)td.Width;
			s_sa.tex_height = td.Height;
			fprintf(stderr, "[DisplayXR-SA] D3D12 shared texture: %ux%u, handle=%p\n",
			        (unsigned)td.Width, td.Height, s_sa.d3d12_shared_handle);

			// Open the shared texture on Unity's device so C# can wrap it
			// with CreateExternalTexture (needs an ID3D12Resource* on Unity's device).
			if (s_unity_d3d12_device && s_sa.d3d12_shared_handle) {
				hr = s_unity_d3d12_device->OpenSharedHandle(
					s_sa.d3d12_shared_handle,
					__uuidof(ID3D12Resource), (void **)&s_sa.d3d12_unity_shared);
				if (SUCCEEDED(hr) && s_sa.d3d12_unity_shared) {
					fprintf(stderr, "[DisplayXR-SA] Opened shared texture on Unity device: %p\n",
					        (void *)s_sa.d3d12_unity_shared);
					s_sa.tex_ready = 1;
				} else {
					fprintf(stderr, "[DisplayXR-SA] OpenSharedHandle on Unity device failed: 0x%08lx\n", hr);
					s_sa.tex_ready = 1; // Still usable on our device
				}
			} else {
				s_sa.tex_ready = 1;
			}
		}
	}
#endif

	// --- Step 8: Create session with Metal graphics binding + window binding ---
	XrSessionCreateInfo session_ci = {XR_TYPE_SESSION_CREATE_INFO};
	session_ci.systemId = s_sa.system_id;

#if defined(__APPLE__)
	// Metal graphics binding (required by the runtime as the primary graphics API)
	XrGraphicsBindingMetalKHR metal_binding = {};
	metal_binding.type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR;
	metal_binding.commandQueue = displayxr_sa_metal_get_command_queue();

	if (!metal_binding.commandQueue) {
		fprintf(stderr, "[DisplayXR-SA] Failed to create MTLCommandQueue\n");
		displayxr_standalone_stop();
		return 0;
	}

	// Cocoa window binding (offscreen with shared IOSurface)
	XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
	mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
	mac_binding.viewHandle = NULL; // offscreen
	mac_binding.readbackCallback = NULL;
	mac_binding.readbackUserdata = NULL;
	mac_binding.sharedIOSurface = displayxr_sa_metal_get_iosurface();

	// Chain: session_ci → metal_binding → mac_binding
	metal_binding.next = &mac_binding;
	session_ci.next = &metal_binding;
#elif defined(_WIN32)
	// Win32 window binding: use the pre-created fullscreen HWND if available
	// (play mode — created by prepare_fullscreen_window() before session start),
	// otherwise fall back to auto-detecting Unity's main window (edit mode preview).
	HWND bind_hwnd = s_fw_hwnd;
	if (!bind_hwnd) {
		DWORD our_pid = GetCurrentProcessId();
		HWND fg = GetForegroundWindow();
		if (fg) {
			DWORD fg_pid = 0;
			GetWindowThreadProcessId(fg, &fg_pid);
			if (fg_pid == our_pid) bind_hwnd = fg;
		}
		if (!bind_hwnd) {
			HWND hw = NULL;
			while ((hw = FindWindowExW(NULL, hw, NULL, NULL)) != NULL) {
				DWORD pid = 0;
				GetWindowThreadProcessId(hw, &pid);
				if (pid == our_pid && IsWindowVisible(hw)) {
					RECT rc;
					if (GetClientRect(hw, &rc) && (rc.right - rc.left) > 100) {
						bind_hwnd = hw;
						break;
					}
				}
			}
		}
	}
	fprintf(stderr, "[DisplayXR-SA] Session HWND: %p (fullscreen=%s)\n",
	        (void *)bind_hwnd, s_fw_hwnd ? "yes" : "no");

	// Binding structs must stay alive across the pfn_create_session call —
	// declare them in the outer scope so they outlive the if/else branches.
	XrGraphicsBindingD3D11KHR d3d11_binding = {};
	XrGraphicsBindingD3D12KHR d3d12_binding = {};
	XrWin32WindowBindingCreateInfoEXT win_binding = {};

	win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	win_binding.windowHandle = (void *)bind_hwnd;
	win_binding.readbackCallback = NULL;
	win_binding.readbackUserdata = NULL;

	if (s_use_d3d11) {
		d3d11_binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
		d3d11_binding.device = s_sa.d3d11_device;
		win_binding.sharedTextureHandle = s_sa.d3d11_shared_handle;
		// Chain: session_ci → d3d11_binding → win_binding
		d3d11_binding.next = &win_binding;
		session_ci.next = &d3d11_binding;
	} else {
		d3d12_binding.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
		d3d12_binding.device = s_sa.d3d12_device;
		d3d12_binding.queue = s_sa.d3d12_queue;
		win_binding.sharedTextureHandle = s_sa.d3d12_shared_handle;
		// Chain: session_ci → d3d12_binding → win_binding
		d3d12_binding.next = &win_binding;
		session_ci.next = &d3d12_binding;
	}
#endif

	result = s_sa.pfn_create_session(s_sa.instance, &session_ci, &s_sa.session);
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrCreateSession failed: %d\n", result);
		displayxr_standalone_stop();
		return 0;
	}
	fprintf(stderr, "[DisplayXR-SA] Session created\n");

	// --- Step 9: Create LOCAL reference space ---
	XrReferenceSpaceCreateInfo space_ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_ci.poseInReferenceSpace.orientation = XrQuaternionf{0, 0, 0, 1};
	space_ci.poseInReferenceSpace.position = XrVector3f{0, 0, 0};

	result = s_sa.pfn_create_reference_space(s_sa.session, &space_ci, &s_sa.local_space);
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrCreateReferenceSpace failed: %d\n", result);
		s_sa.local_space = XR_NULL_HANDLE;
	}

	// --- Step 10: Enumerate rendering modes (need session) ---
	enumerate_and_store_modes();

	// Default to first 3D mode (index 1) if available
	if (s_sa.rendering_mode_count > 1)
		s_sa.current_rendering_mode_index = s_sa.rendering_modes[1].modeIndex;
	else if (s_sa.rendering_mode_count > 0)
		s_sa.current_rendering_mode_index = s_sa.rendering_modes[0].modeIndex;

	// --- Step 11: Create atlas swapchain ---
	if (!create_atlas_swapchain()) {
		fprintf(stderr, "[DisplayXR-SA] Warning: atlas swapchain creation deferred to session ready\n");
	}

	s_sa.running = 1;
	fprintf(stderr, "[DisplayXR-SA] Standalone session started successfully\n");
	return 1;
}


void
displayxr_standalone_stop(void)
{
	displayxr_standalone_fullscreen_window_hide(); // clean up play mode window if showing
	fprintf(stderr, "[DisplayXR-SA] Stopping standalone session\n");
	s_sa.running = 0;
	s_sa.session_ready = 0;

	destroy_atlas_swapchain();

	if (s_sa.local_space != XR_NULL_HANDLE && s_sa.pfn_destroy_space) {
		s_sa.pfn_destroy_space(s_sa.local_space);
		s_sa.local_space = XR_NULL_HANDLE;
	}

	if (s_sa.session != XR_NULL_HANDLE && s_sa.pfn_destroy_session) {
		if (s_sa.session_ready && s_sa.pfn_request_exit_session) {
			s_sa.pfn_request_exit_session(s_sa.session);
			// Drain events for graceful shutdown
			if (s_sa.pfn_poll_event) {
				XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
				for (int i = 0; i < 100; i++) {
					XrResult r = s_sa.pfn_poll_event(s_sa.instance, &event);
					if (r != XR_SUCCESS) break;
					if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
						XrEventDataSessionStateChanged *ssc =
							(XrEventDataSessionStateChanged *)&event;
						if (ssc->state == XR_SESSION_STATE_STOPPING) {
							s_sa.pfn_end_session(s_sa.session);
						}
					}
					event = {XR_TYPE_EVENT_DATA_BUFFER};
				}
			}
		}
		s_sa.pfn_destroy_session(s_sa.session);
		s_sa.session = XR_NULL_HANDLE;
		fprintf(stderr, "[DisplayXR-SA] Session destroyed\n");
	}

#if defined(__APPLE__)
	displayxr_sa_metal_destroy();
#elif defined(_WIN32)
	// D3D11 resources
	if (s_sa.d3d11_unity_atlas_bridge) { s_sa.d3d11_unity_atlas_bridge->Release(); s_sa.d3d11_unity_atlas_bridge = NULL; }
	if (s_sa.d3d11_atlas_bridge_handle) { CloseHandle(s_sa.d3d11_atlas_bridge_handle); s_sa.d3d11_atlas_bridge_handle = NULL; }
	if (s_sa.d3d11_atlas_bridge) { s_sa.d3d11_atlas_bridge->Release(); s_sa.d3d11_atlas_bridge = NULL; }
	if (s_sa.d3d11_unity_shared) { s_sa.d3d11_unity_shared->Release(); s_sa.d3d11_unity_shared = NULL; }
	if (s_sa.d3d11_shared_handle) { CloseHandle(s_sa.d3d11_shared_handle); s_sa.d3d11_shared_handle = NULL; }
	if (s_sa.d3d11_shared_texture) { s_sa.d3d11_shared_texture->Release(); s_sa.d3d11_shared_texture = NULL; }
	if (s_sa.d3d11_context) { s_sa.d3d11_context->Release(); s_sa.d3d11_context = NULL; }
	if (s_sa.d3d11_device) { s_sa.d3d11_device->Release(); s_sa.d3d11_device = NULL; }
	// D3D12 resources
	if (s_sa.d3d12_unity_atlas_bridge) { s_sa.d3d12_unity_atlas_bridge->Release(); s_sa.d3d12_unity_atlas_bridge = NULL; }
	if (s_sa.d3d12_atlas_bridge_handle) { CloseHandle(s_sa.d3d12_atlas_bridge_handle); s_sa.d3d12_atlas_bridge_handle = NULL; }
	if (s_sa.d3d12_atlas_bridge) { s_sa.d3d12_atlas_bridge->Release(); s_sa.d3d12_atlas_bridge = NULL; }
	if (s_sa.d3d12_unity_shared) { s_sa.d3d12_unity_shared->Release(); s_sa.d3d12_unity_shared = NULL; }
	if (s_sa.d3d12_fence_event) { CloseHandle(s_sa.d3d12_fence_event); s_sa.d3d12_fence_event = NULL; }
	if (s_sa.d3d12_fence) { s_sa.d3d12_fence->Release(); s_sa.d3d12_fence = NULL; }
	if (s_sa.d3d12_cmd_list) { s_sa.d3d12_cmd_list->Release(); s_sa.d3d12_cmd_list = NULL; }
	if (s_sa.d3d12_cmd_alloc) { s_sa.d3d12_cmd_alloc->Release(); s_sa.d3d12_cmd_alloc = NULL; }
	if (s_sa.d3d12_shared_texture) { s_sa.d3d12_shared_texture->Release(); s_sa.d3d12_shared_texture = NULL; }
	if (s_sa.d3d12_shared_handle) { CloseHandle(s_sa.d3d12_shared_handle); s_sa.d3d12_shared_handle = NULL; }
	if (s_sa.d3d12_queue) { s_sa.d3d12_queue->Release(); s_sa.d3d12_queue = NULL; }
	if (s_sa.d3d12_device) { s_sa.d3d12_device->Release(); s_sa.d3d12_device = NULL; }
#endif

	if (s_sa.instance != XR_NULL_HANDLE && s_sa.pfn_destroy_instance) {
		s_sa.pfn_destroy_instance(s_sa.instance);
		s_sa.instance = XR_NULL_HANDLE;
		fprintf(stderr, "[DisplayXR-SA] Instance destroyed\n");
	}

	// NOTE: Intentionally skip library unload on both platforms.
	// The runtime may have background threads referencing code in the
	// shared library. Unloading while those are still draining causes crashes.
	// Leaking the handle is harmless — the editor process will reclaim it on
	// exit, and we can re-load on next Start().
	if (s_sa.runtime_lib) {
		// dlclose / FreeLibrary skipped — see comment above
		s_sa.runtime_lib = NULL;
	}

	memset(&s_sa, 0, sizeof(s_sa));
	fprintf(stderr, "[DisplayXR-SA] Standalone session stopped\n");
}


int
displayxr_standalone_is_running(void)
{
	return s_sa.running;
}


// ============================================================================
// Public API: Frame loop
// ============================================================================

void
displayxr_standalone_poll_events(void)
{
	if (!s_sa.running || !s_sa.pfn_poll_event) return;

	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	while (s_sa.pfn_poll_event(s_sa.instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *ssc =
				(XrEventDataSessionStateChanged *)&event;
			s_sa.session_state = ssc->state;

			fprintf(stderr, "[DisplayXR-SA] Session state: %d\n", (int)ssc->state);

			switch (ssc->state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
				begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				XrResult r = s_sa.pfn_begin_session(s_sa.session, &begin_info);
				if (XR_SUCCEEDED(r)) {
					s_sa.session_ready = 1;
					fprintf(stderr, "[DisplayXR-SA] Session begun\n");
					// Create atlas swapchain now if deferred
					if (!s_sa.atlas_created) {
						if (s_sa.rendering_mode_count == 0)
							enumerate_and_store_modes();
						create_atlas_swapchain();
					}
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				s_sa.session_ready = 0;
				s_sa.pfn_end_session(s_sa.session);
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				s_sa.running = 0;
				break;
			default:
				break;
			}
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}


int
displayxr_standalone_begin_frame(int *should_render)
{
	*should_render = 0;
	if (!s_sa.running || !s_sa.session_ready) return 0;

	XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
	XrResult result = s_sa.pfn_wait_frame(s_sa.session, NULL, &frame_state);
	if (XR_FAILED(result)) return 0;

	result = s_sa.pfn_begin_frame(s_sa.session, NULL);
	if (XR_FAILED(result)) return 0;

	s_sa.predicted_display_time = frame_state.predictedDisplayTime;
	s_sa.frame_begun = 1;

	// Locate views (eye tracking — supports N views for multiview modes)
	if (s_sa.local_space != XR_NULL_HANDLE && s_sa.pfn_locate_views) {
		XrViewLocateInfo locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = s_sa.local_space;

		XrView views[SA_MAX_VIEWS];
		for (int i = 0; i < SA_MAX_VIEWS; i++)
			views[i] = {XR_TYPE_VIEW};
		XrViewState view_state = {XR_TYPE_VIEW_STATE};
		uint32_t view_count = 0;

		result = s_sa.pfn_locate_views(s_sa.session, &locate_info,
		                               &view_state, SA_MAX_VIEWS, &view_count, views);
		if (XR_SUCCEEDED(result) && view_count >= 1) {
			if (view_count > SA_MAX_VIEWS) view_count = SA_MAX_VIEWS;
			s_sa.located_view_count = view_count;

			for (uint32_t i = 0; i < view_count; i++) {
				s_sa.eye_positions[i][0] = views[i].pose.position.x;
				s_sa.eye_positions[i][1] = views[i].pose.position.y;
				s_sa.eye_positions[i][2] = views[i].pose.position.z;
			}

			// Backward compat: populate left/right eye from first 2 views
			s_sa.left_eye[0] = views[0].pose.position.x;
			s_sa.left_eye[1] = views[0].pose.position.y;
			s_sa.left_eye[2] = views[0].pose.position.z;
			if (view_count >= 2) {
				s_sa.right_eye[0] = views[1].pose.position.x;
				s_sa.right_eye[1] = views[1].pose.position.y;
				s_sa.right_eye[2] = views[1].pose.position.z;
			} else {
				s_sa.right_eye[0] = views[0].pose.position.x;
				s_sa.right_eye[1] = views[0].pose.position.y;
				s_sa.right_eye[2] = views[0].pose.position.z;
			}

			s_sa.is_tracked = (view_state.viewStateFlags &
			                   XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
		}
	}

	*should_render = frame_state.shouldRender ? 1 : 0;
	return 1;
}


int
displayxr_standalone_submit_frame_atlas(void *atlas_tex)
{
	if (!s_sa.frame_begun || !s_sa.atlas_created) {
		displayxr_standalone_end_frame_empty();
		return 0;
	}
	s_sa.frame_begun = 0;

	// Acquire the single atlas swapchain image
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acq_info = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	XrResult r = s_sa.pfn_acquire_swapchain_image(s_sa.atlas.handle, &acq_info, &index);
	if (XR_FAILED(r)) {
		fprintf(stderr, "[DisplayXR-SA] Acquire atlas swapchain failed: %d\n", r);
		displayxr_standalone_end_frame_empty();
		return 0;
	}

	XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait_info.timeout = 1000000000; // 1 second
	r = s_sa.pfn_wait_swapchain_image(s_sa.atlas.handle, &wait_info);
	if (XR_FAILED(r)) {
		fprintf(stderr, "[DisplayXR-SA] Wait atlas swapchain failed: %d\n", r);
	}

	// Blit Unity atlas RenderTexture → swapchain image (platform-specific)
#if defined(__APPLE__)
	void *dst = s_sa.atlas.images[index].texture;
	if (atlas_tex && dst) {
		displayxr_sa_metal_blit(atlas_tex, dst);
	}
#elif defined(_WIN32)
	if (s_use_d3d11) {
		// D3D11 cross-device atlas blit via shared bridge texture.
		// C# copies Unity's atlas RT → d3d11_unity_atlas_bridge (Unity device).
		// We copy d3d11_atlas_bridge → swapchain image (our device, same device).
		ID3D11Texture2D *bridge = s_sa.d3d11_atlas_bridge;
		ID3D11Texture2D *dst    = s_sa.atlas.images_d3d11[index].texture;
		if (bridge && dst && s_sa.d3d11_context) {
			s_sa.d3d11_context->CopyResource(dst, bridge);
			s_sa.d3d11_context->Flush();
		}
		(void)atlas_tex; // Unused — C# copies to bridge via Graphics.CopyTexture
	} else {
		// D3D12 cross-device atlas blit via shared bridge texture.
		// C# copies Unity's atlas RT → bridge (Unity device, via Graphics.CopyTexture).
		// We copy bridge → swapchain image (our device, same device).
		// Note: content is Y-flipped (Unity D3D12 convention) — the C# display
		// flips the final shared texture output via UV coords.
		ID3D12Resource *bridge = s_sa.d3d12_atlas_bridge;
		ID3D12Resource *dst    = s_sa.atlas.images[index].texture;
		if (bridge && dst && s_sa.d3d12_cmd_list) {
			s_sa.d3d12_cmd_alloc->Reset();
			s_sa.d3d12_cmd_list->Reset(s_sa.d3d12_cmd_alloc, NULL);

			D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
			dst_loc.pResource = dst;
			dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst_loc.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION src_loc = {};
			src_loc.pResource = bridge;
			src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src_loc.SubresourceIndex = 0;

			s_sa.d3d12_cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, NULL);
			s_sa.d3d12_cmd_list->Close();

			ID3D12CommandList *lists[] = { s_sa.d3d12_cmd_list };
			s_sa.d3d12_queue->ExecuteCommandLists(1, lists);

			s_sa.d3d12_fence_value++;
			s_sa.d3d12_queue->Signal(s_sa.d3d12_fence, s_sa.d3d12_fence_value);
			if (s_sa.d3d12_fence->GetCompletedValue() < s_sa.d3d12_fence_value) {
				s_sa.d3d12_fence->SetEventOnCompletion(s_sa.d3d12_fence_value,
				                                        s_sa.d3d12_fence_event);
				WaitForSingleObject(s_sa.d3d12_fence_event, INFINITE);
			}
		}
		(void)atlas_tex; // Unused — C# copies to bridge via Graphics.CopyTexture
	}
#endif

	XrSwapchainImageReleaseInfo rel_info = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_sa.pfn_release_swapchain_image(s_sa.atlas.handle, &rel_info);

	// Get current mode tiling parameters (canvas-aware render dims)
	const XrDisplayRenderingModeInfoEXT *mode = get_current_mode();
	uint32_t eye_count, tile_cols, tile_rows, view_w, view_h;
	get_render_tiling(mode, &eye_count, &tile_cols, &tile_rows, &view_w, &view_h);

	// Determine whether this is a 3D mode (hw3d) or 2D/mono
	int display3D = mode ? (int)mode->hardwareDisplay3D : 1;
	uint32_t n_views = display3D ? eye_count : 1;
	if (n_views > SA_MAX_VIEWS) n_views = SA_MAX_VIEWS;


	// Build raw eye positions for submission (same duplication as compute_views)
	XrVector3f raw_eyes[SA_MAX_VIEWS];
	for (uint32_t i = 0; i < n_views; i++) {
		uint32_t src = (i < s_sa.located_view_count) ? i : 0;
		raw_eyes[i].x = s_sa.eye_positions[src][0];
		raw_eyes[i].y = s_sa.eye_positions[src][1];
		raw_eyes[i].z = s_sa.eye_positions[src][2];
	}

	// For mono mode, average all located eyes to center
	if (!display3D && s_sa.located_view_count >= 2) {
		float cx = 0, cy = 0, cz = 0;
		for (uint32_t i = 0; i < s_sa.located_view_count; i++) {
			cx += s_sa.eye_positions[i][0];
			cy += s_sa.eye_positions[i][1];
			cz += s_sa.eye_positions[i][2];
		}
		float inv = 1.0f / (float)s_sa.located_view_count;
		raw_eyes[0].x = cx * inv;
		raw_eyes[0].y = cy * inv;
		raw_eyes[0].z = cz * inv;
	}

	// Build N projection views with tiled viewports
	XrCompositionLayerProjectionView proj_views[SA_MAX_VIEWS] = {};

	for (uint32_t eye = 0; eye < n_views; eye++) {
		uint32_t tileX = display3D ? (eye % tile_cols) : 0;
		uint32_t tileY = display3D ? (eye / tile_cols) : 0;

		proj_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

		// Use Kooima-computed FOV + eye_world if available (matches test app)
		if (eye < s_sa.computed_view_count) {
			proj_views[eye].fov = s_sa.computed_views[eye].fov;
			proj_views[eye].pose.position = s_sa.computed_views[eye].eye_world;
			proj_views[eye].pose.orientation = s_sa.computed_views[eye].orientation;
		} else {
			proj_views[eye].pose.position = raw_eyes[eye];
			proj_views[eye].pose.orientation = {0, 0, 0, 1};
			proj_views[eye].fov.angleLeft = -0.5f;
			proj_views[eye].fov.angleRight = 0.5f;
			proj_views[eye].fov.angleUp = 0.3f;
			proj_views[eye].fov.angleDown = -0.3f;
		}

		proj_views[eye].subImage.swapchain = s_sa.atlas.handle;
		proj_views[eye].subImage.imageRect.offset = {
			(int32_t)(tileX * view_w), (int32_t)(tileY * view_h)
		};
		proj_views[eye].subImage.imageRect.extent = {
			(int32_t)view_w, (int32_t)view_h
		};
		proj_views[eye].subImage.imageArrayIndex = 0;
	}

	XrCompositionLayerProjection proj_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	proj_layer.space = s_sa.local_space;
	proj_layer.viewCount = n_views;
	proj_layer.views = proj_views;

	const XrCompositionLayerBaseHeader *layers[] = {
		(const XrCompositionLayerBaseHeader *)&proj_layer
	};

	XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = s_sa.predicted_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = 1;
	end_info.layers = layers;

	s_sa.pfn_end_frame(s_sa.session, &end_info);
	return 1;
}


int
displayxr_standalone_submit_frame(void *left_tex, void *right_tex)
{
	// Backward compatibility wrapper — not used by atlas pipeline
	(void)left_tex;
	(void)right_tex;
	if (s_sa.frame_begun) {
		displayxr_standalone_end_frame_empty();
	}
	return 0;
}


void
displayxr_standalone_end_frame_empty(void)
{
	if (!s_sa.frame_begun) return;
	s_sa.frame_begun = 0;

	XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = s_sa.predicted_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = 0;
	end_info.layers = NULL;

	s_sa.pfn_end_frame(s_sa.session, &end_info);
}


// ============================================================================
// Public API: Stereo view computation
// ============================================================================

void
displayxr_standalone_compute_stereo_views(float near_z, float far_z,
                                           float *left_view, float *left_proj,
                                           float *right_view, float *right_proj,
                                           int *valid)
{
	*valid = 0;
	if (!s_sa.display_info.is_valid) {
		static int s_skip_log = 0;
		if (s_skip_log++ % 300 == 0)
			fprintf(stderr, "[DisplayXR-SA] compute_stereo_views skipped: display_valid=%d\n",
				s_sa.display_info.is_valid);
		return;
	}

	XrVector3f raw_left = {s_sa.left_eye[0], s_sa.left_eye[1], s_sa.left_eye[2]};
	XrVector3f raw_right = {s_sa.right_eye[0], s_sa.right_eye[1], s_sa.right_eye[2]};
	XrVector3f nominal = {
		s_sa.display_info.nominal_viewer_x,
		s_sa.display_info.nominal_viewer_y,
		s_sa.display_info.nominal_viewer_z
	};

	// Window-relative Kooima (ADR-012): screen = actual window physical size,
	// eye positions shifted by window-center offset on monitor.
	Display3DScreen screen;
	if (s_sa.canvas_width > 0 && s_sa.canvas_height > 0 &&
	    s_sa.display_info.display_pixel_width > 0 && s_sa.display_info.display_pixel_height > 0) {
		float pxSizeX = s_sa.display_info.display_width_meters / (float)s_sa.display_info.display_pixel_width;
		float pxSizeY = s_sa.display_info.display_height_meters / (float)s_sa.display_info.display_pixel_height;
		screen.width_m = (float)s_sa.canvas_width * pxSizeX;
		screen.height_m = (float)s_sa.canvas_height * pxSizeY;

		// Shift eyes from display-center to window-center coordinates
		float winCenterX = (float)s_sa.canvas_x + (float)s_sa.canvas_width * 0.5f;
		float winCenterY = (float)s_sa.canvas_y + (float)s_sa.canvas_height * 0.5f;
		float dispCenterX = (float)s_sa.display_info.display_pixel_width * 0.5f;
		float dispCenterY = (float)s_sa.display_info.display_pixel_height * 0.5f;
		float eyeOffsetX = (winCenterX - dispCenterX) * pxSizeX;
		float eyeOffsetY = (winCenterY - dispCenterY) * pxSizeY;
#ifdef _WIN32
		eyeOffsetY = -eyeOffsetY; // Win32 Y is top-down, eye coords are Y-up
#endif
		raw_left.x -= eyeOffsetX;
		raw_left.y -= eyeOffsetY;
		raw_right.x -= eyeOffsetX;
		raw_right.y -= eyeOffsetY;
		nominal.x -= eyeOffsetX;
		nominal.y -= eyeOffsetY;
	} else {
		screen.width_m = s_sa.display_info.display_width_meters;
		screen.height_m = s_sa.display_info.display_height_meters;
	}


	// Use stored tunables (from C# DisplayXRDisplay/Camera) or defaults
	Display3DTunables tunables = s_sa.tunables_set
		? s_sa.tunables
		: display3d_default_tunables();

	// Use stored display pose (from parent camera transform) or identity
	XrPosef *pose_ptr = s_sa.display_pose_set ? &s_sa.display_pose : NULL;

	XrVector3f raw_eyes[2] = {raw_left, raw_right};
	Display3DView out_views[2];
	display3d_compute_views(
		raw_eyes, 2, &nominal, &screen, &tunables,
		pose_ptr, near_z, far_z, out_views);

	memcpy(left_view, out_views[0].view_matrix, 16 * sizeof(float));
	memcpy(left_proj, out_views[0].projection_matrix, 16 * sizeof(float));
	memcpy(right_view, out_views[1].view_matrix, 16 * sizeof(float));
	memcpy(right_proj, out_views[1].projection_matrix, 16 * sizeof(float));
	*valid = 1;

	static int s_log_count = 0;
	if (s_log_count++ % 300 == 0) {
		fprintf(stderr, "[DisplayXR-SA] eye_L=(%.3f,%.3f,%.3f) eye_R=(%.3f,%.3f,%.3f) nominal=(%.3f,%.3f,%.3f)\n",
			raw_left.x, raw_left.y, raw_left.z,
			raw_right.x, raw_right.y, raw_right.z,
			nominal.x, nominal.y, nominal.z);
		fprintf(stderr, "[DisplayXR-SA] screen=(%.3f x %.3f)m near=%.3f far=%.1f\n",
			screen.width_m, screen.height_m, near_z, far_z);
		fprintf(stderr, "[DisplayXR-SA] L_view: [%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
			left_view[0], left_view[4], left_view[8], left_view[12],
			left_view[1], left_view[5], left_view[9], left_view[13],
			left_view[2], left_view[6], left_view[10], left_view[14],
			left_view[3], left_view[7], left_view[11], left_view[15]);
		fprintf(stderr, "[DisplayXR-SA] L_proj: [%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
			left_proj[0], left_proj[4], left_proj[8], left_proj[12],
			left_proj[1], left_proj[5], left_proj[9], left_proj[13],
			left_proj[2], left_proj[6], left_proj[10], left_proj[14],
			left_proj[3], left_proj[7], left_proj[11], left_proj[15]);
	}
}


// ============================================================================
// Public API: Multiview compute (N views)
// ============================================================================

void
displayxr_standalone_compute_views(
	uint32_t view_count,
	float near_z, float far_z,
	float *view_matrices,
	float *proj_matrices,
	int *valid)
{
	*valid = 0;
	if (!s_sa.display_info.is_valid || view_count == 0) return;

	// Window-relative Kooima (ADR-012): screen = actual window physical size,
	// eye positions shifted by window-center offset on monitor.
	Display3DScreen screen;
	float eyeOffsetX_mv = 0.0f, eyeOffsetY_mv = 0.0f;
	if (s_sa.canvas_width > 0 && s_sa.canvas_height > 0 &&
	    s_sa.display_info.display_pixel_width > 0 && s_sa.display_info.display_pixel_height > 0) {
		float pxSizeX = s_sa.display_info.display_width_meters / (float)s_sa.display_info.display_pixel_width;
		float pxSizeY = s_sa.display_info.display_height_meters / (float)s_sa.display_info.display_pixel_height;
		screen.width_m = (float)s_sa.canvas_width * pxSizeX;
		screen.height_m = (float)s_sa.canvas_height * pxSizeY;

		// Shift eyes from display-center to window-center coordinates
		float winCenterX = (float)s_sa.canvas_x + (float)s_sa.canvas_width * 0.5f;
		float winCenterY = (float)s_sa.canvas_y + (float)s_sa.canvas_height * 0.5f;
		float dispCenterX = (float)s_sa.display_info.display_pixel_width * 0.5f;
		float dispCenterY = (float)s_sa.display_info.display_pixel_height * 0.5f;
		eyeOffsetX_mv = (winCenterX - dispCenterX) * pxSizeX;
		eyeOffsetY_mv = (winCenterY - dispCenterY) * pxSizeY;
#ifdef _WIN32
		eyeOffsetY_mv = -eyeOffsetY_mv; // Win32 Y is top-down, eye coords are Y-up
#endif
	} else {
		screen.width_m = s_sa.display_info.display_width_meters;
		screen.height_m = s_sa.display_info.display_height_meters;
	}

	Display3DTunables tunables = s_sa.tunables_set
		? s_sa.tunables
		: display3d_default_tunables();

	XrPosef *pose_ptr = s_sa.display_pose_set ? &s_sa.display_pose : NULL;

	XrVector3f nominal = {
		s_sa.display_info.nominal_viewer_x - eyeOffsetX_mv,
		s_sa.display_info.nominal_viewer_y - eyeOffsetY_mv,
		s_sa.display_info.nominal_viewer_z
	};

	// Build raw eye positions for all requested views.
	// If mode needs more views than xrLocateViews returned (e.g. quad=4
	// but PRIMARY_STEREO=2), duplicate views[0] for extras — matches the
	// reference test app.
	XrVector3f raw_eyes[SA_MAX_VIEWS];
	uint32_t n = view_count;
	if (n > SA_MAX_VIEWS) n = SA_MAX_VIEWS;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t src = (i < s_sa.located_view_count) ? i : 0;
		raw_eyes[i].x = s_sa.eye_positions[src][0] - eyeOffsetX_mv;
		raw_eyes[i].y = s_sa.eye_positions[src][1] - eyeOffsetY_mv;
		raw_eyes[i].z = s_sa.eye_positions[src][2];
	}

	if (s_sa.camera_centric) {
		// Camera-centric: use camera3d library (tangent-space Kooima)
		Camera3DTunables cam_tunables;
		cam_tunables.ipd_factor = tunables.ipd_factor;
		cam_tunables.parallax_factor = tunables.parallax_factor;
		cam_tunables.inv_convergence_distance = s_sa.inv_convergence_distance;
		cam_tunables.half_tan_vfov = s_sa.fov_override;

		Camera3DView cam_views[SA_MAX_VIEWS];
		camera3d_compute_views(
			raw_eyes, n, &nominal, &screen, &cam_tunables,
			pose_ptr, near_z, far_z, cam_views);

		for (uint32_t i = 0; i < n; i++) {
			memcpy(&view_matrices[i * 16], cam_views[i].view_matrix, 16 * sizeof(float));
			memcpy(&proj_matrices[i * 16], cam_views[i].projection_matrix, 16 * sizeof(float));
		}

		// Cache as Display3DView for submit_frame_atlas (layout-compatible fields)
		for (uint32_t i = 0; i < n; i++) {
			memcpy(s_sa.computed_views[i].view_matrix, cam_views[i].view_matrix, 16 * sizeof(float));
			memcpy(s_sa.computed_views[i].projection_matrix, cam_views[i].projection_matrix, 16 * sizeof(float));
			s_sa.computed_views[i].fov = cam_views[i].fov;
			s_sa.computed_views[i].eye_world = cam_views[i].eye_world;
			s_sa.computed_views[i].orientation = cam_views[i].orientation;
		}
		s_sa.computed_view_count = n;
	} else {
		// Display-centric: use display3d library
		Display3DView out_views[SA_MAX_VIEWS];
		display3d_compute_views(
			raw_eyes, n, &nominal, &screen, &tunables,
			pose_ptr, near_z, far_z, out_views);

		for (uint32_t i = 0; i < n; i++) {
			memcpy(&view_matrices[i * 16], out_views[i].view_matrix, 16 * sizeof(float));
			memcpy(&proj_matrices[i * 16], out_views[i].projection_matrix, 16 * sizeof(float));
		}

		// Cache computed views for submit_frame_atlas (FOV + eye_world)
		memcpy(s_sa.computed_views, out_views, n * sizeof(Display3DView));
		s_sa.computed_view_count = n;
	}

	*valid = 1;
}


// ============================================================================
// Public API: Current mode info
// ============================================================================

void
displayxr_standalone_get_current_mode_info(
	uint32_t *view_count,
	uint32_t *tile_columns, uint32_t *tile_rows,
	uint32_t *view_width_pixels, uint32_t *view_height_pixels,
	float *view_scale_x, float *view_scale_y,
	int *hardware_display_3d)
{
	const XrDisplayRenderingModeInfoEXT *mode = get_current_mode();
	uint32_t vc, tc, tr, vw, vh;
	get_render_tiling(mode, &vc, &tc, &tr, &vw, &vh);

	*view_count = vc;
	*tile_columns = tc;
	*tile_rows = tr;
	*view_width_pixels = vw;
	*view_height_pixels = vh;
	*view_scale_x = mode ? mode->viewScaleX : 1.0f;
	*view_scale_y = mode ? mode->viewScaleY : 1.0f;
	*hardware_display_3d = mode ? (int)mode->hardwareDisplay3D : 0;
}


// ============================================================================
// Public API: Tunables + display pose
// ============================================================================

void
displayxr_standalone_set_tunables(
	float ipd_factor, float parallax_factor, float perspective_factor,
	float virtual_display_height, float inv_convergence_distance, float fov_override,
	float near_z, float far_z, int camera_centric)
{
	s_sa.tunables.ipd_factor = ipd_factor;
	s_sa.tunables.parallax_factor = parallax_factor;
	s_sa.tunables.perspective_factor = perspective_factor;
	s_sa.tunables.virtual_display_height = virtual_display_height;
	s_sa.tunables_set = 1;
	s_sa.camera_centric = camera_centric;
	s_sa.inv_convergence_distance = inv_convergence_distance;
	s_sa.fov_override = fov_override;
	// near_z, far_z come from the C# compute_views call directly
	(void)near_z;
	(void)far_z;
}

void
displayxr_standalone_set_display_pose(
	float pos_x, float pos_y, float pos_z,
	float ori_x, float ori_y, float ori_z, float ori_w,
	float scale_x, float scale_y, float scale_z,
	int enabled)
{
	if (enabled) {
		// Convert Unity coords (left-hand, +Z forward) to OpenXR (right-hand, -Z forward):
		// negate position Z, negate quaternion X/Y.
		// Matches hooked_xrLocateViews convention in displayxr_hooks.cpp.
		s_sa.display_pose.position = XrVector3f{pos_x, pos_y, -pos_z};
		s_sa.display_pose.orientation = XrQuaternionf{-ori_x, -ori_y, ori_z, ori_w};
		s_sa.display_pose_set = 1;
		// scale is folded into virtual_display_height by C# side (like the hook chain)
		(void)scale_x;
		(void)scale_y;
		(void)scale_z;
	} else {
		s_sa.display_pose_set = 0;
	}
}


// ============================================================================
// Public API: Queries
// ============================================================================

// Keep old poll() as a convenience wrapper for backwards compat
void
displayxr_standalone_poll(void)
{
	displayxr_standalone_poll_events();
	int should_render = 0;
	if (displayxr_standalone_begin_frame(&should_render)) {
		displayxr_standalone_end_frame_empty();
	}
}


void
displayxr_standalone_get_display_info(float *display_width_m, float *display_height_m,
                                       uint32_t *pixel_width, uint32_t *pixel_height,
                                       float *nominal_x, float *nominal_y, float *nominal_z,
                                       float *scale_x, float *scale_y,
                                       int *is_valid)
{
	DisplayXRDisplayInfo *di = &s_sa.display_info;
	*display_width_m = di->display_width_meters;
	*display_height_m = di->display_height_meters;
	*pixel_width = di->display_pixel_width;
	*pixel_height = di->display_pixel_height;
	*nominal_x = di->nominal_viewer_x;
	*nominal_y = di->nominal_viewer_y;
	*nominal_z = di->nominal_viewer_z;
	*scale_x = di->recommended_view_scale_x;
	*scale_y = di->recommended_view_scale_y;
	*is_valid = di->is_valid;
}


void
displayxr_standalone_get_eye_positions(float *lx, float *ly, float *lz,
                                        float *rx, float *ry, float *rz,
                                        int *is_tracked)
{
	*lx = s_sa.left_eye[0];
	*ly = s_sa.left_eye[1];
	*lz = s_sa.left_eye[2];
	*rx = s_sa.right_eye[0];
	*ry = s_sa.right_eye[1];
	*rz = s_sa.right_eye[2];
	*is_tracked = s_sa.is_tracked;
}


void
displayxr_standalone_get_atlas_bridge_texture(void **native_ptr,
                                               uint32_t *width, uint32_t *height)
{
#if defined(_WIN32)
	if (s_use_d3d11) {
		*native_ptr = s_sa.d3d11_unity_atlas_bridge
			? (void *)s_sa.d3d11_unity_atlas_bridge : NULL;
	} else {
		*native_ptr = s_sa.d3d12_unity_atlas_bridge
			? (void *)s_sa.d3d12_unity_atlas_bridge : NULL;
	}
	*width = s_sa.atlas.width;
	*height = s_sa.atlas.height;
#else
	*native_ptr = NULL;
	*width = 0;
	*height = 0;
#endif
}


void
displayxr_standalone_get_shared_texture(void **native_ptr, uint32_t *width,
                                         uint32_t *height, int *ready)
{
#if defined(__APPLE__)
	*native_ptr = displayxr_sa_metal_get_texture();
#elif defined(_WIN32)
	// Return the shared texture opened on Unity's device (if available),
	// otherwise fall back to our device's resource.
	if (s_use_d3d11) {
		*native_ptr = s_sa.d3d11_unity_shared
			? (void *)s_sa.d3d11_unity_shared
			: (void *)s_sa.d3d11_shared_texture;
	} else {
		*native_ptr = s_sa.d3d12_unity_shared
			? (void *)s_sa.d3d12_unity_shared
			: (void *)s_sa.d3d12_shared_texture;
	}
#else
	*native_ptr = NULL;
#endif
	*width = s_sa.tex_width;
	*height = s_sa.tex_height;
	*ready = s_sa.tex_ready;
}


#if defined(__APPLE__)
extern "C" float displayxr_sa_metal_get_backing_scale(void);
#endif

float
displayxr_get_backing_scale_factor(void)
{
#if defined(__APPLE__)
	return displayxr_sa_metal_get_backing_scale();
#elif defined(_WIN32)
	UINT dpi = GetDpiForSystem();
	static int logged = 0;
	if (!logged) {
		fprintf(stderr, "[DisplayXR-SA] GetDpiForSystem=%u, backingScale=%.2f\n",
		        dpi, (float)dpi / 96.0f);
		logged = 1;
	}
	return (float)dpi / 96.0f;
#else
	return 1.0f;
#endif
}


void
displayxr_standalone_set_canvas_rect(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	static uint32_t log_count = 0;
	if (log_count < 5 || (w != s_sa.canvas_width || h != s_sa.canvas_height)) {
		fprintf(stderr, "[DisplayXR-SA] set_canvas_rect: x=%d y=%d w=%u h=%u "
		        "(shared_tex=%ux%u, atlas=%ux%u, display=%ux%u)\n",
		        x, y, w, h, s_sa.tex_width, s_sa.tex_height,
		        s_sa.atlas.width, s_sa.atlas.height,
		        s_sa.display_info.display_pixel_width,
		        s_sa.display_info.display_pixel_height);
		log_count++;
	}
	s_sa.canvas_width = w;
	s_sa.canvas_height = h;
	s_sa.canvas_x = x;
	s_sa.canvas_y = y;
	if (s_sa.pfn_set_output_rect && s_sa.session != XR_NULL_HANDLE)
		s_sa.pfn_set_output_rect(s_sa.session, x, y, w, h);
}


void
displayxr_standalone_get_swapchain_size(uint32_t *width, uint32_t *height)
{
	if (s_sa.atlas_created) {
		*width = s_sa.atlas.width;
		*height = s_sa.atlas.height;
	} else if (s_sa.display_info.is_valid) {
		// Estimate atlas size from display info
		*width = s_sa.display_info.display_pixel_width * 2;
		*height = s_sa.display_info.display_pixel_height;
	} else {
		*width = 0;
		*height = 0;
	}
}


// ============================================================================
// Public API: Display mode switching
// ============================================================================

int
displayxr_standalone_request_display_mode(int mode_3d)
{
	if (!s_sa.has_display_mode_ext || !s_sa.pfn_request_display_mode ||
	    s_sa.session == XR_NULL_HANDLE)
		return 0;

	XrDisplayModeEXT mode = mode_3d ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
	XrResult result = s_sa.pfn_request_display_mode(s_sa.session, mode);
	fprintf(stderr, "[DisplayXR-SA] RequestDisplayMode(%s) → %d\n",
		mode_3d ? "3D" : "2D", result);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

int
displayxr_standalone_request_rendering_mode(uint32_t mode_index)
{
	if (!s_sa.pfn_request_rendering_mode || s_sa.session == XR_NULL_HANDLE)
		return 0;

	XrResult result = s_sa.pfn_request_rendering_mode(s_sa.session, mode_index);
	fprintf(stderr, "[DisplayXR-SA] RequestRenderingMode(%u) → %d\n",
		mode_index, result);
	if (XR_SUCCEEDED(result)) {
		s_sa.current_rendering_mode_index = mode_index;
		return 1;
	}
	return 0;
}

int
displayxr_standalone_enumerate_rendering_modes(
	uint32_t capacity, uint32_t *count,
	uint32_t *mode_indices, char (*mode_names)[256],
	uint32_t *view_counts,
	uint32_t *tile_columns, uint32_t *tile_rows,
	uint32_t *view_width_pixels, uint32_t *view_height_pixels,
	float *view_scale_x, float *view_scale_y,
	int *hardware_display_3d)
{
	// Use cached modes if available, otherwise query runtime
	uint32_t total = s_sa.rendering_mode_count;
	if (total == 0) {
		if (!s_sa.pfn_enumerate_rendering_modes || s_sa.session == XR_NULL_HANDLE) {
			*count = 0;
			return 0;
		}
		// Re-enumerate
		enumerate_and_store_modes();
		total = s_sa.rendering_mode_count;
	}

	if (total == 0) {
		*count = 0;
		return 0;
	}

	*count = total;
	if (capacity == 0 || !mode_indices || !mode_names)
		return 1; // Count-only query

	uint32_t to_fetch = total < capacity ? total : capacity;
	for (uint32_t i = 0; i < to_fetch; i++) {
		mode_indices[i] = s_sa.rendering_modes[i].modeIndex;
		strncpy(mode_names[i], s_sa.rendering_modes[i].modeName, 255);
		mode_names[i][255] = '\0';

		// Extended fields (NULL-safe — callers may pass NULL for fields they don't need)
		if (view_counts) view_counts[i] = s_sa.rendering_modes[i].viewCount;
		if (tile_columns) tile_columns[i] = s_sa.rendering_modes[i].tileColumns;
		if (tile_rows) tile_rows[i] = s_sa.rendering_modes[i].tileRows;
		if (view_width_pixels) view_width_pixels[i] = s_sa.rendering_modes[i].viewWidthPixels;
		if (view_height_pixels) view_height_pixels[i] = s_sa.rendering_modes[i].viewHeightPixels;
		if (view_scale_x) view_scale_x[i] = s_sa.rendering_modes[i].viewScaleX;
		if (view_scale_y) view_scale_y[i] = s_sa.rendering_modes[i].viewScaleY;
		if (hardware_display_3d) hardware_display_3d[i] = (int)s_sa.rendering_modes[i].hardwareDisplay3D;
	}
	return 1;
}


// ============================================================================
// Public API: Fullscreen window on 3D display (play mode)
//
// The HWND is created BEFORE the standalone session starts (via
// prepare_fullscreen_window) so it can be passed to xrCreateSession via
// XrWin32WindowBindingCreateInfoEXT.  The runtime then owns presentation:
// it weaves and blits directly to the HWND as part of xrEndFrame.
// present() only needs to pump Win32 messages for Escape detection.
// ============================================================================

#if defined(_WIN32)

static void fw_toggle_fullscreen(void);              // forward decl
static void fw_resize_swapchain(uint32_t w, uint32_t h); // forward decl

static LRESULT CALLBACK
fw_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_KEYDOWN && wp == VK_ESCAPE)  { s_fw_escape_pressed = 1; return 0; }
	if (msg == WM_KEYDOWN && wp == VK_F11)     { fw_toggle_fullscreen(); return 0; }
	if (msg == WM_CLOSE)                        { s_fw_escape_pressed = 1; return 0; }
	if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
		uint32_t w = LOWORD(lp);
		uint32_t h = HIWORD(lp);
		if (w > 0 && h > 0) fw_resize_swapchain(w, h);
		return 0;
	}
	if (msg == WM_GETMINMAXINFO) {
		MINMAXINFO *mmi = (MINMAXINFO *)lp;
		// Enforce minimum 500×500 client area, adjusted for window decorations.
		RECT rc = { 0, 0, 500, 500 };
		DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
		DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
		AdjustWindowRectEx(&rc, style, FALSE, exstyle);
		mmi->ptMinTrackSize.x = rc.right  - rc.left;
		mmi->ptMinTrackSize.y = rc.bottom - rc.top;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// Find the best monitor for the fullscreen window.
// Prefers a non-primary monitor (Unity editor is typically on the primary).
// If disp_w/disp_h are non-zero, also tries to match that exact resolution.
struct FWMonitorFind {
	uint32_t target_w, target_h; // 0 = accept any size
	HMONITOR found;
	RECT     found_rect;
};

static BOOL CALLBACK
fw_find_monitor_cb(HMONITOR hm, HDC, LPRECT, LPARAM lp)
{
	FWMonitorFind *f = (FWMonitorFind *)lp;
	MONITORINFO mi = { sizeof(mi) };
	if (!GetMonitorInfo(hm, &mi)) return TRUE;
	LONG mw = mi.rcMonitor.right  - mi.rcMonitor.left;
	LONG mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
	bool size_match = (f->target_w == 0 && f->target_h == 0) ||
	                  ((uint32_t)mw == f->target_w && (uint32_t)mh == f->target_h);
	if (!size_match) return TRUE;
	bool is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	// Accept if nothing found yet, or if this is non-primary (preferred)
	if (!f->found || !is_primary) {
		f->found      = hm;
		f->found_rect = mi.rcMonitor;
	}
	return TRUE;
}

static BOOL CALLBACK
fw_count_monitors_cb(HMONITOR, HDC, LPRECT, LPARAM lp)
{
	(*(int *)lp)++;
	return TRUE;
}

static int
fw_count_monitors(void)
{
	int n = 0;
	EnumDisplayMonitors(NULL, NULL, fw_count_monitors_cb, (LPARAM)&n);
	return n;
}

// Toggle the window between borderless fullscreen (on the 3D monitor) and
// a windowed mode. Saves/restores windowed rect so the window returns to its
// previous position when going back to windowed.
static void
fw_toggle_fullscreen(void)
{
	if (!s_fw_hwnd) return;

	if (s_fw_is_fullscreen) {
		// Go windowed: restore resizable style and saved rect
		SetWindowLongW(s_fw_hwnd, GWL_STYLE,   WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowLongW(s_fw_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
		SetWindowPos(s_fw_hwnd, HWND_TOP,
		             s_fw_windowed_rect.left,
		             s_fw_windowed_rect.top,
		             s_fw_windowed_rect.right  - s_fw_windowed_rect.left,
		             s_fw_windowed_rect.bottom - s_fw_windowed_rect.top,
		             SWP_FRAMECHANGED | SWP_NOACTIVATE);
		s_fw_is_fullscreen = false;
		// Resize swap chain to match restored windowed client area
		RECT client = {};
		GetClientRect(s_fw_hwnd, &client);
		uint32_t cw = (uint32_t)(client.right  - client.left);
		uint32_t ch = (uint32_t)(client.bottom - client.top);
		if (cw > 0 && ch > 0) fw_resize_swapchain(cw, ch);
		fprintf(stderr, "[DisplayXR-FW] toggle: windowed\n");
	} else {
		// Go fullscreen: save current rect, find 3D monitor, cover it
		GetWindowRect(s_fw_hwnd, &s_fw_windowed_rect);

		uint32_t disp_w = s_sa.display_info.display_pixel_width;
		uint32_t disp_h = s_sa.display_info.display_pixel_height;
		FWMonitorFind finder = { disp_w, disp_h, NULL, {} };
		if (disp_w > 0 && disp_h > 0)
			EnumDisplayMonitors(NULL, NULL, fw_find_monitor_cb, (LPARAM)&finder);
		if (!finder.found) {
			FWMonitorFind fb = { 0, 0, NULL, {} };
			EnumDisplayMonitors(NULL, NULL, fw_find_monitor_cb, (LPARAM)&fb);
			finder = fb;
		}
		if (!finder.found) return;

		RECT mr = finder.found_rect;
		SetWindowLongW(s_fw_hwnd, GWL_STYLE,   WS_POPUP | WS_VISIBLE);
		SetWindowLongW(s_fw_hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_APPWINDOW);
		SetWindowPos(s_fw_hwnd, HWND_TOPMOST,
		             mr.left, mr.top,
		             mr.right - mr.left, mr.bottom - mr.top,
		             SWP_FRAMECHANGED | SWP_NOACTIVATE);
		s_fw_is_fullscreen = true;
		// Resize swap chain to full display dimensions
		uint32_t disp_w2 = s_sa.display_info.display_pixel_width;
		uint32_t disp_h2 = s_sa.display_info.display_pixel_height;
		if (disp_w2 > 0 && disp_h2 > 0) fw_resize_swapchain(disp_w2, disp_h2);
		fprintf(stderr, "[DisplayXR-FW] toggle: fullscreen on (%ld,%ld)\n",
		        mr.left, mr.top);
	}
}

static HWND
fw_create_window(RECT mr)
{
	int win_w = (int)(mr.right  - mr.left);
	int win_h = (int)(mr.bottom - mr.top);

	WNDCLASSEXW wc   = { sizeof(wc) };
	wc.lpfnWndProc   = fw_wndproc;
	wc.hInstance     = GetModuleHandleW(NULL);
	wc.lpszClassName = L"DisplayXR_Fullscreen";
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassExW(&wc); // ignore already-exists error

	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	// Create hidden — fullscreen_window_show() moves it to the correct monitor
	// and calls ShowWindow() once display info is available.
	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_APPWINDOW,
		L"DisplayXR_Fullscreen", L"DisplayXR 3D",
		WS_POPUP,
		mr.left, mr.top, win_w, win_h,
		NULL, NULL, GetModuleHandleW(NULL), NULL);
	return hwnd;
}


static void
fw_destroy_swapchain(void)
{
	if (s_fw_bb11[0])   { s_fw_bb11[0]->Release();   s_fw_bb11[0]   = NULL; }
	if (s_fw_bb11[1])   { s_fw_bb11[1]->Release();   s_fw_bb11[1]   = NULL; }
	if (s_fw_cmd_list)  { s_fw_cmd_list->Release();  s_fw_cmd_list  = NULL; }
	if (s_fw_cmd_alloc) { s_fw_cmd_alloc->Release(); s_fw_cmd_alloc = NULL; }
	if (s_fw_bb[0])     { s_fw_bb[0]->Release();     s_fw_bb[0]     = NULL; }
	if (s_fw_bb[1])     { s_fw_bb[1]->Release();     s_fw_bb[1]     = NULL; }
	if (s_fw_swapchain) { s_fw_swapchain->Release(); s_fw_swapchain = NULL; }
	if (s_fw_fence)     { s_fw_fence->Release();     s_fw_fence     = NULL; }
	if (s_fw_fence_event) { CloseHandle(s_fw_fence_event); s_fw_fence_event = NULL; }
	s_fw_fence_value = 0;
	s_fw_sc_w = 0;
	s_fw_sc_h = 0;
}

// Create the DXGI swap chain on s_fw_hwnd at the given pixel dimensions.
// Called from fullscreen_window_show() after display info is available.
static bool
fw_create_swapchain(uint32_t w, uint32_t h)
{
	if (!s_fw_hwnd) return false;

	IDXGIFactory4 *factory = NULL;
	if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory))) {
		fprintf(stderr, "[DisplayXR-FW] CreateDXGIFactory2 failed\n");
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.BufferCount  = 2;
	sd.Width        = w;
	sd.Height       = h;
	sd.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.SampleDesc.Count = 1;
	sd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	IDXGISwapChain1 *sc1 = NULL;
	HRESULT hr;

	if (s_use_d3d11) {
		if (!s_sa.d3d11_device) { factory->Release(); return false; }
		hr = factory->CreateSwapChainForHwnd(
		    s_sa.d3d11_device, s_fw_hwnd, &sd, NULL, NULL, &sc1);
	} else {
		if (!s_sa.d3d12_device || !s_sa.d3d12_queue) { factory->Release(); return false; }
		hr = factory->CreateSwapChainForHwnd(
		    s_sa.d3d12_queue, s_fw_hwnd, &sd, NULL, NULL, &sc1);
	}
	factory->MakeWindowAssociation(s_fw_hwnd, DXGI_MWA_NO_ALT_ENTER);
	factory->Release();
	if (FAILED(hr)) {
		fprintf(stderr, "[DisplayXR-FW] CreateSwapChainForHwnd failed: 0x%08lx\n", hr);
		return false;
	}
	sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&s_fw_swapchain);
	sc1->Release();

	if (s_use_d3d11) {
		for (UINT i = 0; i < 2; i++)
			s_fw_swapchain->GetBuffer(i, __uuidof(ID3D11Texture2D), (void **)&s_fw_bb11[i]);
		// No command list/fence needed for D3D11 immediate context
		s_fw_sc_w = w;
		s_fw_sc_h = h;
		fprintf(stderr, "[DisplayXR-FW] D3D11 swap chain created: %ux%u\n", w, h);
	} else {
		for (UINT i = 0; i < 2; i++)
			s_fw_swapchain->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&s_fw_bb[i]);

		s_sa.d3d12_device->CreateCommandAllocator(
		    D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
		    (void **)&s_fw_cmd_alloc);
		s_sa.d3d12_device->CreateCommandList(
		    0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_fw_cmd_alloc, NULL,
		    __uuidof(ID3D12GraphicsCommandList), (void **)&s_fw_cmd_list);
		s_fw_cmd_list->Close();

		s_fw_fence_value = 0;
		s_sa.d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		    __uuidof(ID3D12Fence), (void **)&s_fw_fence);
		s_fw_fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

		s_fw_sc_w = w;
		s_fw_sc_h = h;
		fprintf(stderr, "[DisplayXR-FW] D3D12 swap chain created: %ux%u\n", w, h);
	}
	return true;
}

// Resize swap chain back buffers after a window resize.
// GPU must be idle before calling (ensured by present()'s fence wait).
static void
fw_resize_swapchain(uint32_t w, uint32_t h)
{
	if (!s_fw_swapchain) return;
	if (w == s_fw_sc_w && h == s_fw_sc_h) return;

	// Release back buffer references before ResizeBuffers
	if (s_fw_bb11[0]) { s_fw_bb11[0]->Release(); s_fw_bb11[0] = NULL; }
	if (s_fw_bb11[1]) { s_fw_bb11[1]->Release(); s_fw_bb11[1] = NULL; }
	if (s_fw_bb[0])   { s_fw_bb[0]->Release();   s_fw_bb[0]   = NULL; }
	if (s_fw_bb[1])   { s_fw_bb[1]->Release();   s_fw_bb[1]   = NULL; }

	HRESULT hr = s_fw_swapchain->ResizeBuffers(
	    2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
	if (FAILED(hr)) {
		fprintf(stderr, "[DisplayXR-FW] ResizeBuffers failed: 0x%08lx\n", hr);
		return;
	}

	if (s_use_d3d11) {
		for (UINT i = 0; i < 2; i++)
			s_fw_swapchain->GetBuffer(i, __uuidof(ID3D11Texture2D), (void **)&s_fw_bb11[i]);
	} else {
		for (UINT i = 0; i < 2; i++)
			s_fw_swapchain->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&s_fw_bb[i]);
	}

	s_fw_sc_w = w;
	s_fw_sc_h = h;

	// Update canvas so the runtime weaves at the new size
	uint32_t disp_w = s_sa.display_info.display_pixel_width;
	uint32_t disp_h = s_sa.display_info.display_pixel_height;
	// Clamp to display dimensions (can't weave larger than the shared texture)
	uint32_t cw = (w <= disp_w) ? w : disp_w;
	uint32_t ch = (h <= disp_h) ? h : disp_h;
	displayxr_standalone_set_canvas_rect(0, 0, cw, ch);
	fprintf(stderr, "[DisplayXR-FW] resized swap chain: %ux%u canvas: %ux%u\n",
	        w, h, cw, ch);
}

#endif // _WIN32


// Called BEFORE displayxr_standalone_start() in play mode.
// Creates the fullscreen HWND on the non-primary monitor so the session can
// be created with it as the window binding (runtime presents directly to it).
DISPLAYXR_EXPORT int
displayxr_standalone_prepare_fullscreen_window(void)
{
#if defined(_WIN32)
	if (s_fw_hwnd) return 1; // already prepared

	// Find non-primary monitor (3D display). No display info needed yet.
	FWMonitorFind finder = { 0, 0, NULL, {} };
	EnumDisplayMonitors(NULL, NULL, fw_find_monitor_cb, (LPARAM)&finder);
	if (!finder.found) {
		// Only one monitor — use primary
		POINT pt = {0, 0};
		finder.found = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi = { sizeof(mi) };
		GetMonitorInfo(finder.found, &mi);
		finder.found_rect = mi.rcMonitor;
	}
	fprintf(stderr, "[DisplayXR-FW] prepare: monitor (%ld,%ld) %ldx%ld\n",
	        finder.found_rect.left, finder.found_rect.top,
	        finder.found_rect.right  - finder.found_rect.left,
	        finder.found_rect.bottom - finder.found_rect.top);

	s_fw_hwnd = fw_create_window(finder.found_rect);
	if (!s_fw_hwnd) {
		fprintf(stderr, "[DisplayXR-FW] prepare: CreateWindow failed (%lu)\n",
		        GetLastError());
		return 0;
	}
	s_fw_escape_pressed = 0;
	fprintf(stderr, "[DisplayXR-FW] prepare: HWND=%p ready for session binding\n",
	        (void *)s_fw_hwnd);
	return 1;
#else
	return 0;
#endif
}


// Called from GameViewOverlay once the session is running and display info
// is available.  Sets canvas_rect to the full display so the runtime weaves
// at correct pixel alignment.  HWND was already created by prepare().
DISPLAYXR_EXPORT int
displayxr_standalone_fullscreen_window_show(void)
{
#if defined(_WIN32)
	if (!s_fw_hwnd) return 0; // prepare_fullscreen_window() was not called

	if (!s_sa.display_info.is_valid) {
		static int s_log = 0;
		if (s_log++ < 3)
			fprintf(stderr, "[DisplayXR-FW] show: waiting for display info\n");
		return 0;
	}

	uint32_t disp_w = s_sa.display_info.display_pixel_width;
	uint32_t disp_h = s_sa.display_info.display_pixel_height;

	int monitor_count = fw_count_monitors();

	// sw_w/sw_h = swap chain / canvas dimensions for this mode
	uint32_t sc_w, sc_h;

	if (monitor_count > 1) {
		// Multiple monitors: open fullscreen on the 3D display.
		// Match by display pixel dimensions; fall back to non-primary.
		FWMonitorFind finder = { disp_w, disp_h, NULL, {} };
		EnumDisplayMonitors(NULL, NULL, fw_find_monitor_cb, (LPARAM)&finder);
		if (!finder.found) {
			FWMonitorFind fb = { 0, 0, NULL, {} };
			EnumDisplayMonitors(NULL, NULL, fw_find_monitor_cb, (LPARAM)&fb);
			finder = fb;
		}
		if (finder.found) {
			RECT mr = finder.found_rect;
			SetWindowLongW(s_fw_hwnd, GWL_STYLE,   WS_POPUP | WS_VISIBLE);
			SetWindowLongW(s_fw_hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_APPWINDOW);
			SetWindowPos(s_fw_hwnd, HWND_TOPMOST,
			             mr.left, mr.top,
			             mr.right - mr.left, mr.bottom - mr.top,
			             SWP_FRAMECHANGED | SWP_NOACTIVATE);
			fprintf(stderr, "[DisplayXR-FW] show: fullscreen on (%ld,%ld) %ldx%ld\n",
			        mr.left, mr.top,
			        mr.right - mr.left, mr.bottom - mr.top);
		}
		s_fw_is_fullscreen = true;
		sc_w = disp_w;
		sc_h = disp_h;
	} else {
		// Single monitor: open resizable windowed window at half display size, centered.
		uint32_t win_w = disp_w / 2;
		uint32_t win_h = disp_h / 2;
		int cx = GetSystemMetrics(SM_CXSCREEN);
		int cy = GetSystemMetrics(SM_CYSCREEN);
		int x  = (cx - (int)win_w) / 2;
		int y  = (cy - (int)win_h) / 2;
		SetWindowLongW(s_fw_hwnd, GWL_STYLE,   WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowLongW(s_fw_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
		SetWindowPos(s_fw_hwnd, HWND_TOP, x, y, (int)win_w, (int)win_h,
		             SWP_FRAMECHANGED | SWP_NOACTIVATE);
		GetWindowRect(s_fw_hwnd, &s_fw_windowed_rect);
		s_fw_is_fullscreen = false;
		// Use client area for swap chain dimensions
		RECT client = {};
		GetClientRect(s_fw_hwnd, &client);
		sc_w = (uint32_t)(client.right  - client.left);
		sc_h = (uint32_t)(client.bottom - client.top);
		fprintf(stderr, "[DisplayXR-FW] show: windowed client=%ux%u\n", sc_w, sc_h);
	}

	// Create D3D12 swap chain sized to the initial window client area.
	if (!s_fw_swapchain) {
		if (!fw_create_swapchain(sc_w, sc_h)) {
			fprintf(stderr, "[DisplayXR-FW] show: swap chain creation failed\n");
			return 0;
		}
	}

	displayxr_standalone_set_canvas_rect(0, 0, sc_w, sc_h);
	ShowWindow(s_fw_hwnd, SW_SHOW);
	SetForegroundWindow(s_fw_hwnd);
	fprintf(stderr, "[DisplayXR-FW] show: canvas=%ux%u\n", disp_w, disp_h);
	return 1;
#else
	return 0;
#endif
}


DISPLAYXR_EXPORT void
displayxr_standalone_fullscreen_window_hide(void)
{
#if defined(_WIN32)
	fw_destroy_swapchain();
	if (s_fw_hwnd) {
		MSG msg;
		while (PeekMessage(&msg, s_fw_hwnd, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
		DestroyWindow(s_fw_hwnd);
		s_fw_hwnd = NULL;
	}
	s_fw_escape_pressed = 0;
	s_fw_is_fullscreen  = false;
	fprintf(stderr, "[DisplayXR-FW] Fullscreen window destroyed\n");
#endif
}


// Blit the weaved shared texture to the HWND and present.
// The runtime writes to s_sa.d3d12_shared_texture in xrEndFrame (shared texture
// mode), then returns. We copy that to the DXGI swap chain back buffer and Present.
DISPLAYXR_EXPORT void
displayxr_standalone_fullscreen_window_present(void)
{
#if defined(_WIN32)
	if (!s_fw_hwnd) return;

	// Pump Win32 messages (Escape detection, resize, etc.)
	MSG msg;
	while (PeekMessage(&msg, s_fw_hwnd, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// Blit shared texture → swap chain back buffer → Present
	if (!s_fw_swapchain) return;

	UINT bb_idx = s_fw_swapchain->GetCurrentBackBufferIndex();

	if (s_use_d3d11) {
		// D3D11 blit: CopySubresourceRegion canvas region → back buffer
		ID3D11Texture2D *src = s_sa.d3d11_shared_texture;
		ID3D11Texture2D *bb  = s_fw_bb11[bb_idx];
		if (!src || !bb || !s_sa.d3d11_context) return;

		D3D11_BOX box = { 0, 0, 0, s_fw_sc_w, s_fw_sc_h, 1 };
		s_sa.d3d11_context->CopySubresourceRegion(bb, 0, 0, 0, 0, src, 0, &box);
		s_sa.d3d11_context->Flush();
	} else {
		// D3D12 blit via command list
		if (!s_sa.d3d12_shared_texture) return;
		ID3D12Resource *bb = s_fw_bb[bb_idx];

		s_fw_cmd_alloc->Reset();
		s_fw_cmd_list->Reset(s_fw_cmd_alloc, NULL);

		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource  = s_sa.d3d12_shared_texture;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource  = bb;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		s_fw_cmd_list->ResourceBarrier(2, barriers);

		// Copy only the canvas-sized content region from the shared texture.
		// The shared texture is always disp_w×disp_h but the runtime only writes
		// in the top-left s_fw_sc_w×s_fw_sc_h area (= current canvas).
		{
			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource        = s_sa.d3d12_shared_texture;
			src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource        = bb;
			dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = 0;

			D3D12_BOX box = { 0, 0, 0, s_fw_sc_w, s_fw_sc_h, 1 };
			s_fw_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		}

		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
		s_fw_cmd_list->ResourceBarrier(2, barriers);

		s_fw_cmd_list->Close();
		ID3D12CommandList *lists[] = { s_fw_cmd_list };
		s_sa.d3d12_queue->ExecuteCommandLists(1, lists);

		// Wait for GPU completion before Present (fence on same queue as runtime)
		s_fw_fence_value++;
		s_sa.d3d12_queue->Signal(s_fw_fence, s_fw_fence_value);
		if (s_fw_fence->GetCompletedValue() < s_fw_fence_value) {
			s_fw_fence->SetEventOnCompletion(s_fw_fence_value, s_fw_fence_event);
			WaitForSingleObject(s_fw_fence_event, INFINITE);
		}
	}

	s_fw_swapchain->Present(0, 0);
#endif
}


DISPLAYXR_EXPORT int
displayxr_standalone_fullscreen_window_escape_pressed(void)
{
	int v = s_fw_escape_pressed;
	s_fw_escape_pressed = 0;
	return v;
}


// ============================================================================
// Standalone graphics backend factory
// ============================================================================

static StandaloneGraphicsBackend *create_standalone_backend(void)
{
#if defined(_WIN32)
  #if defined(ENABLE_VULKAN)
	return create_standalone_vulkan_backend();
  #elif defined(ENABLE_OPENGL)
	return create_standalone_opengles_backend();
  #else
	// Runtime choice: D3D11 or D3D12 based on Unity's active API.
	// s_use_d3d11 is set by displayxr_standalone_set_unity_device().
	if (s_use_d3d11)
		return create_standalone_d3d11_backend();
	else
		return create_standalone_d3d12_backend();
  #endif
#elif defined(__APPLE__)
  #if defined(ENABLE_OPENGL)
	return create_standalone_opengl_backend();
  #else
	return create_standalone_metal_backend();
  #endif
#elif defined(__ANDROID__)
  #if defined(ENABLE_OPENGL)
	return create_standalone_opengles_backend();
  #else
	return create_standalone_vulkan_backend();
  #endif
#else  // Linux and other Unix
  #if defined(ENABLE_OPENGL)
	return create_standalone_opengl_backend();
  #else
	return create_standalone_vulkan_backend();
  #endif
#endif
}
