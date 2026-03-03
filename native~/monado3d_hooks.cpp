// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// OpenXR function interception layer for the Monado3D Unity plugin.
// Hooks into Unity's OpenXR loader chain via HookGetInstanceProcAddr.

#include "monado3d_hooks.h"
#include "monado3d_extensions.h"
#include "monado3d_kooima.h"
#include "monado3d_shared_state.h"

#include <string.h>
#include <stdio.h>

// --- Stored real function pointers ---
static PFN_xrGetInstanceProcAddr s_next_gipa = nullptr;
static PFN_xrLocateViews s_real_locate_views = nullptr;
static PFN_xrGetSystemProperties s_real_get_system_properties = nullptr;
static PFN_xrCreateSession s_real_create_session = nullptr;
static PFN_xrEndFrame s_real_end_frame = nullptr;
static PFN_xrCreateReferenceSpace s_real_create_reference_space = nullptr;

static XrInstance s_instance = XR_NULL_HANDLE;
static XrSession s_session = XR_NULL_HANDLE;
static XrSpace s_display_space = XR_NULL_HANDLE;


// ============================================================================
// Intercepted OpenXR functions
// ============================================================================

static XrResult XRAPI_CALL
hooked_xrLocateViews(XrSession session,
                     const XrViewLocateInfo *viewLocateInfo,
                     XrViewState *viewState,
                     uint32_t viewCapacityInput,
                     uint32_t *viewCountOutput,
                     XrView *views)
{
	// Use DISPLAY space if available, otherwise pass through the original space
	XrViewLocateInfo modified_info = *viewLocateInfo;
	if (s_display_space != XR_NULL_HANDLE) {
		modified_info.space = s_display_space;
	}

	// Call the real xrLocateViews
	XrResult result = s_real_locate_views(session, &modified_info, viewState, viewCapacityInput, viewCountOutput,
	                                      views);
	if (XR_FAILED(result) || viewCapacityInput < 2 || views == nullptr) {
		return result;
	}

	uint32_t count = *viewCountOutput;
	if (count < 2) {
		return result;
	}

	// Cache raw eye positions for C# access (before any transforms)
	uint8_t tracked = (viewState->viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
	monado3d_state_set_eye_positions(&views[0].pose.position, &views[1].pose.position, tracked);

	// Get current tunables, scene transform, and display info
	Monado3DTunables tunables = monado3d_state_get_tunables();
	Monado3DSceneTransform scene_xform = monado3d_state_get_scene_transform();
	Monado3DState *state = monado3d_get_state();
	Monado3DDisplayInfo *di = &state->display_info;

	if (!di->is_valid) {
		return result; // No display info — pass through unmodified
	}

	// Transform chain: raw → scene transform → tunables → Kooima
	//
	// Step 1: Apply scene transform (parent camera pose, zoom)
	// This mirrors the test app's player transform applied before Kooima.
	XrVector3f scene_left, scene_right;
	monado3d_apply_scene_transform(&views[0].pose.position, &views[1].pose.position, &scene_xform,
	                               &scene_left, &scene_right);

	// Step 2: Apply tunables (IPD, parallax, perspective, scale)
	XrVector3f mod_left, mod_right;
	monado3d_apply_tunables(&scene_left, &scene_right, &tunables, di, &mod_left, &mod_right);

	// Determine screen extents
	float screen_w, screen_h;
	if (tunables.camera_centric && tunables.convergence_distance > 0.0f) {
		// Camera-centric: compute virtual screen extents
		monado3d_camera_centric_extents(tunables.convergence_distance, tunables.fov_override, di, &screen_w,
		                                &screen_h);
	} else {
		// Display-centric: use physical extents scaled by scale factor
		screen_w = di->display_width_meters * tunables.scale_factor;
		screen_h = di->display_height_meters * tunables.scale_factor;
	}

	// Compute Kooima asymmetric frustum FOVs
	XrFovf left_fov = monado3d_compute_kooima_fov(mod_left, screen_w, screen_h);
	XrFovf right_fov = monado3d_compute_kooima_fov(mod_right, screen_w, screen_h);

	// Write modified FOVs back — Unity will use these for projection matrices
	views[0].fov = left_fov;
	views[1].fov = right_fov;

	// Write modified poses back
	views[0].pose.position = mod_left;
	views[1].pose.position = mod_right;

	return result;
}

static XrResult XRAPI_CALL
hooked_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties *properties)
{
	// Call real function first
	XrResult result = s_real_get_system_properties(instance, systemId, properties);
	if (XR_FAILED(result)) {
		return result;
	}

	// Walk the next chain looking for XrDisplayInfoEXT
	void *next = properties->next;
	while (next != nullptr) {
		XrBaseOutStructure *base = (XrBaseOutStructure *)next;
		if (base->type == XR_TYPE_DISPLAY_INFO_EXT) {
			XrDisplayInfoEXT *di = (XrDisplayInfoEXT *)base;
			Monado3DState *state = monado3d_get_state();

			state->display_info.display_width_meters = di->displaySizeMeters.width;
			state->display_info.display_height_meters = di->displaySizeMeters.height;
			state->display_info.display_pixel_width = di->displayPixelWidth;
			state->display_info.display_pixel_height = di->displayPixelHeight;
			state->display_info.nominal_viewer_x = di->nominalViewerPositionInDisplaySpace.x;
			state->display_info.nominal_viewer_y = di->nominalViewerPositionInDisplaySpace.y;
			state->display_info.nominal_viewer_z = di->nominalViewerPositionInDisplaySpace.z;
			state->display_info.recommended_view_scale_x = di->recommendedViewScaleX;
			state->display_info.recommended_view_scale_y = di->recommendedViewScaleY;
			state->display_info.supports_display_mode_switch = di->supportsDisplayModeSwitch ? 1 : 0;
			state->display_info.is_valid = 1;

			// Look up display mode function
			if (di->supportsDisplayModeSwitch && s_next_gipa && s_instance) {
				PFN_xrVoidFunction fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrRequestDisplayModeEXT", &fn))) {
					state->pfn_request_display_mode = (PFN_xrRequestDisplayModeEXT)fn;
					state->has_display_mode_ext = 1;
				}
			}
			break;
		}
		next = (void *)base->next;
	}

	return result;
}

static XrResult XRAPI_CALL
hooked_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session)
{
	Monado3DState *state = monado3d_get_state();

	// If we have a window handle, inject window binding into the next chain
	if (state->window_handle != nullptr) {
		// Walk the chain to find the graphics binding (last item before NULL)
		const XrBaseInStructure *chain = (const XrBaseInStructure *)createInfo->next;
		const XrBaseInStructure *last_in_chain = nullptr;

		while (chain != nullptr) {
			last_in_chain = chain;
			chain = chain->next;
		}

		if (last_in_chain != nullptr) {
#if defined(_WIN32)
			// Inject Win32 window binding at end of chain
			static XrWin32WindowBindingCreateInfoEXT win_binding = {};
			win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
			win_binding.next = nullptr;
			win_binding.windowHandle = state->window_handle;

			// Append to chain (cast away const — we own the lifetime)
			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&win_binding;
#elif defined(__APPLE__)
			static XrMacOSWindowBindingCreateInfoEXT mac_binding = {};
			mac_binding.type = XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT;
			mac_binding.next = nullptr;
			mac_binding.viewHandle = state->window_handle;
			mac_binding.readbackCallback = nullptr;
			mac_binding.readbackUserdata = nullptr;

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&mac_binding;
#endif
		}
	}

	XrResult result = s_real_create_session(instance, createInfo, session);
	if (XR_SUCCEEDED(result)) {
		s_session = *session;

		// Try to create DISPLAY reference space
		if (s_real_create_reference_space != nullptr) {
			XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT;
			space_info.poseInReferenceSpace.orientation = {0, 0, 0, 1};
			space_info.poseInReferenceSpace.position = {0, 0, 0};

			XrResult space_result = s_real_create_reference_space(s_session, &space_info, &s_display_space);
			if (XR_FAILED(space_result)) {
				s_display_space = XR_NULL_HANDLE;
			}
		}
	}

	return result;
}

static XrResult XRAPI_CALL
hooked_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
	Monado3DState *state = monado3d_get_state();

	// Count active window-space layers
	int active_layers = 0;
	for (int i = 0; i < MONADO3D_MAX_WINDOW_LAYERS; i++) {
		if (state->window_layers[i].active && state->window_layers[i].swapchain != XR_NULL_HANDLE) {
			active_layers++;
		}
	}

	if (active_layers == 0) {
		// No overlay layers — pass through
		return s_real_end_frame(session, frameEndInfo);
	}

	// Build extended layer array: original layers + window-space layers
	uint32_t total = frameEndInfo->layerCount + (uint32_t)active_layers;
	const XrCompositionLayerBaseHeader **layers = new const XrCompositionLayerBaseHeader *[total];

	// Copy original layers
	for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
		layers[i] = frameEndInfo->layers[i];
	}

	// Append window-space layers
	static XrCompositionLayerWindowSpaceEXT ws_layers[MONADO3D_MAX_WINDOW_LAYERS] = {};
	uint32_t idx = frameEndInfo->layerCount;
	for (int i = 0; i < MONADO3D_MAX_WINDOW_LAYERS; i++) {
		Monado3DWindowLayer *wl = &state->window_layers[i];
		if (!wl->active || wl->swapchain == XR_NULL_HANDLE) {
			continue;
		}

		ws_layers[i].type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
		ws_layers[i].next = nullptr;
		ws_layers[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		ws_layers[i].subImage.swapchain = wl->swapchain;
		ws_layers[i].subImage.imageRect.offset = {0, 0};
		ws_layers[i].subImage.imageRect.extent = {(int32_t)wl->swapchain_width,
		                                           (int32_t)wl->swapchain_height};
		ws_layers[i].subImage.imageArrayIndex = 0;
		ws_layers[i].x = wl->x;
		ws_layers[i].y = wl->y;
		ws_layers[i].width = wl->width;
		ws_layers[i].height = wl->height;
		ws_layers[i].disparity = wl->disparity;

		layers[idx++] = (const XrCompositionLayerBaseHeader *)&ws_layers[i];
	}

	// Submit with extended layers
	XrFrameEndInfo modified = *frameEndInfo;
	modified.layerCount = total;
	modified.layers = layers;

	XrResult result = s_real_end_frame(session, &modified);
	delete[] layers;
	return result;
}


// ============================================================================
// Hook installation — called by Unity's OpenXR Feature
// ============================================================================

XrResult XRAPI_CALL
monado3d_hook_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function)
{
	// First get the real function from the next in chain
	XrResult result = s_next_gipa(instance, name, function);
	if (XR_FAILED(result)) {
		return result;
	}

	// Cache instance handle
	if (instance != XR_NULL_HANDLE) {
		s_instance = instance;
	}

	// Intercept specific functions
	if (strcmp(name, "xrLocateViews") == 0) {
		s_real_locate_views = (PFN_xrLocateViews)*function;
		*function = (PFN_xrVoidFunction)hooked_xrLocateViews;
	} else if (strcmp(name, "xrGetSystemProperties") == 0) {
		s_real_get_system_properties = (PFN_xrGetSystemProperties)*function;
		*function = (PFN_xrVoidFunction)hooked_xrGetSystemProperties;
	} else if (strcmp(name, "xrCreateSession") == 0) {
		s_real_create_session = (PFN_xrCreateSession)*function;
		*function = (PFN_xrVoidFunction)hooked_xrCreateSession;
	} else if (strcmp(name, "xrEndFrame") == 0) {
		s_real_end_frame = (PFN_xrEndFrame)*function;
		*function = (PFN_xrVoidFunction)hooked_xrEndFrame;
	} else if (strcmp(name, "xrCreateReferenceSpace") == 0) {
		s_real_create_reference_space = (PFN_xrCreateReferenceSpace)*function;
		// Don't intercept — just cache the function pointer
	}

	return result;
}

PFN_xrVoidFunction
monado3d_install_hooks(PFN_xrGetInstanceProcAddr next_gipa)
{
	monado3d_state_init();
	s_next_gipa = next_gipa;
	return (PFN_xrVoidFunction)monado3d_hook_xrGetInstanceProcAddr;
}


// ============================================================================
// P/Invoke exports
// ============================================================================

void
monado3d_set_tunables(float ipd_factor,
                      float parallax_factor,
                      float perspective_factor,
                      float scale_factor,
                      float convergence_distance,
                      float fov_override,
                      int camera_centric)
{
	Monado3DTunables t;
	t.ipd_factor = ipd_factor;
	t.parallax_factor = parallax_factor;
	t.perspective_factor = perspective_factor;
	t.scale_factor = scale_factor;
	t.convergence_distance = convergence_distance;
	t.fov_override = fov_override;
	t.camera_centric = camera_centric ? 1 : 0;
	monado3d_state_set_tunables(&t);
}

void
monado3d_get_display_info(float *display_width_m,
                          float *display_height_m,
                          uint32_t *pixel_width,
                          uint32_t *pixel_height,
                          float *nominal_x,
                          float *nominal_y,
                          float *nominal_z,
                          float *scale_x,
                          float *scale_y,
                          int *supports_mode_switch,
                          int *is_valid)
{
	Monado3DState *state = monado3d_get_state();
	Monado3DDisplayInfo *di = &state->display_info;

	*display_width_m = di->display_width_meters;
	*display_height_m = di->display_height_meters;
	*pixel_width = di->display_pixel_width;
	*pixel_height = di->display_pixel_height;
	*nominal_x = di->nominal_viewer_x;
	*nominal_y = di->nominal_viewer_y;
	*nominal_z = di->nominal_viewer_z;
	*scale_x = di->recommended_view_scale_x;
	*scale_y = di->recommended_view_scale_y;
	*supports_mode_switch = di->supports_display_mode_switch;
	*is_valid = di->is_valid;
}

void
monado3d_get_eye_positions(float *lx, float *ly, float *lz, float *rx, float *ry, float *rz, int *is_tracked)
{
	Monado3DEyePositions eyes = monado3d_state_get_eye_positions();
	*lx = eyes.left_eye.x;
	*ly = eyes.left_eye.y;
	*lz = eyes.left_eye.z;
	*rx = eyes.right_eye.x;
	*ry = eyes.right_eye.y;
	*rz = eyes.right_eye.z;
	*is_tracked = eyes.is_tracked;
}

void
monado3d_set_window_handle(void *handle)
{
	Monado3DState *state = monado3d_get_state();
	state->window_handle = handle;
}

int
monado3d_request_display_mode(int mode_3d)
{
	Monado3DState *state = monado3d_get_state();
	if (!state->has_display_mode_ext || state->pfn_request_display_mode == nullptr || s_session == XR_NULL_HANDLE) {
		return 0; // Not supported
	}

	XrDisplayModeEXT mode = mode_3d ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
	XrResult result = state->pfn_request_display_mode(s_session, mode);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

void
monado3d_set_scene_transform(float pos_x,
                             float pos_y,
                             float pos_z,
                             float ori_x,
                             float ori_y,
                             float ori_z,
                             float ori_w,
                             float zoom_scale,
                             int enabled)
{
	Monado3DSceneTransform t;
	t.position[0] = pos_x;
	t.position[1] = pos_y;
	t.position[2] = pos_z;
	t.orientation[0] = ori_x;
	t.orientation[1] = ori_y;
	t.orientation[2] = ori_z;
	t.orientation[3] = ori_w;
	t.zoom_scale = zoom_scale;
	t.enabled = enabled ? 1 : 0;
	monado3d_state_set_scene_transform(&t);
}

void
monado3d_get_readback(uint8_t **pixels, uint32_t *width, uint32_t *height, int *ready)
{
	Monado3DState *state = monado3d_get_state();
	*pixels = state->readback_pixels;
	*width = state->readback_width;
	*height = state->readback_height;
	*ready = state->readback_ready;
}
