// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenXR function interception layer for the DisplayXR Unity plugin.
// Hooks into Unity's OpenXR loader chain via HookGetInstanceProcAddr.

#include "displayxr_hooks.h"
#include "displayxr_extensions.h"
#include "displayxr_kooima.h"
#include "displayxr_shared_state.h"
#include "displayxr_readback.h"
#include "camera3d_view.h"
#include <math.h>

#if defined(__APPLE__)
#include "displayxr_metal.h"
#elif defined(_WIN32)
#include "displayxr_win32.h"
#endif

#include <string.h>
#include <stdio.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

// --- Stored real function pointers ---
static PFN_xrGetInstanceProcAddr s_next_gipa = nullptr;
static PFN_xrLocateViews s_real_locate_views = nullptr;
static PFN_xrGetSystemProperties s_real_get_system_properties = nullptr;
static PFN_xrCreateSession s_real_create_session = nullptr;
static PFN_xrDestroySession s_real_destroy_session = nullptr;
static PFN_xrEndFrame s_real_end_frame = nullptr;
static PFN_xrCreateReferenceSpace s_real_create_reference_space = nullptr;
static volatile PFN_xrPollEvent s_real_poll_event = nullptr;
static PFN_xrDestroyInstance s_real_destroy_instance = nullptr;

static XrInstance s_instance = XR_NULL_HANDLE;
static XrSession s_session = XR_NULL_HANDLE;
static XrSpace s_local_space = XR_NULL_HANDLE;
static volatile int s_session_alive = 0; // Guard for teardown
static volatile int s_instance_alive = 0; // Guard for post-destroy polling

// --- Deferred destruction ---
// Unity's OpenXR loader calls xrPollEvent AFTER xrDestroyInstance returns,
// through JIT-generated dispatch trampolines that reference runtime memory.
// If we actually destroy the instance, those trampolines read freed pages → SIGSEGV.
// Fix: defer the real destroy calls until the next instance lifecycle begins.
static XrSession s_deferred_destroy_session = XR_NULL_HANDLE;
static PFN_xrDestroySession s_deferred_destroy_session_fn = nullptr;
static XrInstance s_deferred_destroy_instance = XR_NULL_HANDLE;
static PFN_xrDestroyInstance s_deferred_destroy_instance_fn = nullptr;
static int s_runtime_pinned = 0; // Whether we've pinned the runtime via RTLD_NODELETE
static volatile int s_stop_polling = 0; // Stop forwarding xrPollEvent after EXITING event


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
	// Guard: skip if session is being torn down
	if (!s_session_alive) {
		return s_real_locate_views(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput,
		                           views);
	}

	// Use our LOCAL space if available, otherwise pass through the original space.
	// LOCAL space gives us raw eye positions relative to the display.
	XrViewLocateInfo modified_info = *viewLocateInfo;
	if (s_local_space != XR_NULL_HANDLE) {
		modified_info.space = s_local_space;
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
	displayxr_state_set_eye_positions(&views[0].pose.position, &views[1].pose.position, tracked);


	// Get current tunables, scene transform, and display info
	DisplayXRTunables tunables = displayxr_state_get_tunables();
	DisplayXRSceneTransform scene_xform = displayxr_state_get_scene_transform();
	DisplayXRState *state = displayxr_get_state();
	DisplayXRDisplayInfo *di = &state->display_info;

	if (!di->is_valid) {
		static int s_no_di_count = 0;
		if (s_no_di_count++ % 60 == 0) {
			fprintf(stderr, "[DisplayXR] xrLocateViews: display_info NOT valid, passing through raw views "
			        "(raw_L=(%.3f,%.3f,%.3f) raw_R=(%.3f,%.3f,%.3f))\n",
			        views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
			        views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z);
		}
		return result; // No display info — pass through unmodified
	}

	// Delegate to canonical view libraries for IPD/parallax/Kooima.
	// Raw eye positions and scene transform (camera/display pose) are passed
	// to the libraries, matching the native test app's pipeline exactly.
	XrVector3f raw_left = views[0].pose.position;
	XrVector3f raw_right = views[1].pose.position;
	XrVector3f nominal = {di->nominal_viewer_x, di->nominal_viewer_y, di->nominal_viewer_z};

	// Adjust Kooima screen dimensions to match the viewport (window) aspect ratio.
	// Without this, resizing the window stretches the scene because the Kooima frustum
	// always uses the physical display's aspect ratio. The test app does the same
	// adjustment: converts window pixels to meters, then normalizes so the min
	// dimension matches the display's min dimension.
	Display3DScreen screen = {di->display_width_meters, di->display_height_meters};
	if (state->viewport_width > 0 && state->viewport_height > 0 &&
	    di->display_pixel_width > 0 && di->display_pixel_height > 0) {
		float px_size_x = di->display_width_meters / (float)di->display_pixel_width;
		float px_size_y = di->display_height_meters / (float)di->display_pixel_height;
		float vp_w_m = (float)state->viewport_width * px_size_x;
		float vp_h_m = (float)state->viewport_height * px_size_y;
		float min_disp = fminf(di->display_width_meters, di->display_height_meters);
		float min_vp = fminf(vp_w_m, vp_h_m);
		if (min_vp > 0.0001f) {
			float vs = min_disp / min_vp;
			screen.width_m = vp_w_m * vs;
			screen.height_m = vp_h_m * vs;
		}
	}

	// Build pose from scene transform (Unity camera/display world pose).
	// Convert Unity coords (left-hand, +Z forward) to OpenXR (right-hand, -Z forward):
	// position Z negated, quaternion (x,y) negated + (z,w) kept.
	XrPosef scene_pose = {};
	if (scene_xform.enabled) {
		scene_pose.position = XrVector3f{
			scene_xform.position[0],
			scene_xform.position[1],
			-scene_xform.position[2]};
		scene_pose.orientation = XrQuaternionf{
			-scene_xform.orientation[0],
			-scene_xform.orientation[1],
			scene_xform.orientation[2],
			scene_xform.orientation[3]};
	} else {
		scene_pose.orientation = XrQuaternionf{0, 0, 0, 1};
		scene_pose.position = XrVector3f{0, 0, 0};
	}

	if (tunables.camera_centric) {
		// Camera-centric: tangent-based Kooima (camera3d_view library)
		// scene_pose = Unity camera world pose converted to OpenXR coords.
		static int s_cam_log = 0;
		if (s_cam_log++ % 60 == 0) {
			fprintf(stderr, "[DisplayXR] CAM-CENTRIC: scene_pose=(%.3f,%.3f,%.3f) "
			        "raw_L=(%.3f,%.3f,%.3f) raw_R=(%.3f,%.3f,%.3f) "
			        "nominal=(%.3f,%.3f,%.3f) invd=%.4f half_tan_vfov=%.4f\n",
			        scene_pose.position.x, scene_pose.position.y, scene_pose.position.z,
			        raw_left.x, raw_left.y, raw_left.z,
			        raw_right.x, raw_right.y, raw_right.z,
			        nominal.x, nominal.y, nominal.z,
			        tunables.inv_convergence_distance,
			        tunables.fov_override);
		}
		Camera3DTunables cam_tunables;
		cam_tunables.ipd_factor = tunables.ipd_factor;
		cam_tunables.parallax_factor = tunables.parallax_factor;
		cam_tunables.inv_convergence_distance = tunables.inv_convergence_distance;
		cam_tunables.half_tan_vfov = tunables.fov_override;

		XrVector3f raw_eyes[2] = {raw_left, raw_right};
		Camera3DView cam_views[2];
		camera3d_compute_views(
			raw_eyes, 2,
			&nominal, &screen, &cam_tunables,
			&scene_pose,
			tunables.near_z, tunables.far_z,
			cam_views);

		views[0].fov = cam_views[0].fov;
		views[1].fov = cam_views[1].fov;

		views[0].pose.position = cam_views[0].eye_world;
		views[1].pose.position = cam_views[1].eye_world;
		views[0].pose.orientation = scene_pose.orientation;
		views[1].pose.orientation = scene_pose.orientation;

		// Store Kooima matrices for C# stereo override
		DisplayXRStereoMatrices mats = {};
		memcpy(mats.left_view, cam_views[0].view_matrix, sizeof(float) * 16);
		memcpy(mats.left_projection, cam_views[0].projection_matrix, sizeof(float) * 16);
		memcpy(mats.right_view, cam_views[1].view_matrix, sizeof(float) * 16);
		memcpy(mats.right_projection, cam_views[1].projection_matrix, sizeof(float) * 16);
		mats.valid = 1;
		displayxr_state_set_stereo_matrices(&mats);

		if (s_cam_log % 60 == 1) {
			fprintf(stderr, "[DisplayXR] OUTPUT L: eye_world=(%.3f,%.3f,%.3f) "
			        "fov=(L=%.1f R=%.1f U=%.1f D=%.1f)\n",
			        cam_views[0].eye_world.x, cam_views[0].eye_world.y, cam_views[0].eye_world.z,
			        cam_views[0].fov.angleLeft * 57.2958f, cam_views[0].fov.angleRight * 57.2958f,
			        cam_views[0].fov.angleUp * 57.2958f, cam_views[0].fov.angleDown * 57.2958f);
		}
	} else {
		// Display-centric: atan-based Kooima (display3d_view library)
		// Pass raw eyes + display_pose directly (like the native test app).
		//
		// Camera transform scale acts as zoom via virtual_display_height:
		// virtual_display_height is divided by scale.y so a larger scale
		// means a smaller virtual display → zooms in (like test app mouse wheel).

		float sy = (scene_xform.scale[1] > 0.001f) ? scene_xform.scale[1] : 1.0f;
		float vdh = tunables.virtual_display_height;
		// Fold camera scale into virtual display height
		vdh /= sy;

		Display3DTunables disp_tunables;
		disp_tunables.ipd_factor = tunables.ipd_factor;
		disp_tunables.parallax_factor = tunables.parallax_factor;
		disp_tunables.perspective_factor = tunables.perspective_factor;
		disp_tunables.virtual_display_height = vdh;

		XrVector3f raw_eyes[2] = {raw_left, raw_right};
		Display3DView disp_views[2];
		display3d_compute_views(
			raw_eyes, 2,
			&nominal, &screen, &disp_tunables,
			scene_xform.enabled ? &scene_pose : NULL,
			tunables.near_z, tunables.far_z,
			disp_views);

		views[0].fov = disp_views[0].fov;
		views[1].fov = disp_views[1].fov;
		views[0].pose.position = disp_views[0].eye_world;
		views[1].pose.position = disp_views[1].eye_world;
		views[0].pose.orientation = scene_pose.orientation;
		views[1].pose.orientation = scene_pose.orientation;

		// Store Kooima matrices for C# stereo override
		DisplayXRStereoMatrices mats = {};
		memcpy(mats.left_view, disp_views[0].view_matrix, sizeof(float) * 16);
		memcpy(mats.left_projection, disp_views[0].projection_matrix, sizeof(float) * 16);
		memcpy(mats.right_view, disp_views[1].view_matrix, sizeof(float) * 16);
		memcpy(mats.right_projection, disp_views[1].projection_matrix, sizeof(float) * 16);
		mats.valid = 1;
		displayxr_state_set_stereo_matrices(&mats);
	}

	// Debug: log every 60 frames (AFTER writeback so we see final values)
	static int s_frame_count = 0;
	if (s_frame_count++ % 60 == 0) {
		float l_hfov = (views[0].fov.angleRight - views[0].fov.angleLeft) * 57.2958f;
		float l_vfov = (views[0].fov.angleUp - views[0].fov.angleDown) * 57.2958f;
		fprintf(stderr,
		        "[DisplayXR] FINALv2: cam_centric=%d "
		        "pos_L=(%.4f,%.4f,%.4f) pos_R=(%.4f,%.4f,%.4f) "
		        "fov_L=(%.2f,%.2f,%.2f,%.2f)deg hfov=%.1f vfov=%.1f "
		        "ori_L=(%.3f,%.3f,%.3f,%.3f)\n",
		        tunables.camera_centric,
		        views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
		        views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z,
		        views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f,
		        views[0].fov.angleUp * 57.2958f, views[0].fov.angleDown * 57.2958f,
		        l_hfov, l_vfov,
		        views[0].pose.orientation.x, views[0].pose.orientation.y,
		        views[0].pose.orientation.z, views[0].pose.orientation.w);
	}

	return result;
}

static XrResult XRAPI_CALL
hooked_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties *properties)
{
	// Inject XrDisplayInfoEXT into the next chain so the runtime fills it in.
	// Unity's OpenXR loader doesn't know about this extension, so we chain it ourselves.
	static XrDisplayInfoEXT display_info_ext = {};
	display_info_ext.type = XR_TYPE_DISPLAY_INFO_EXT;
	display_info_ext.next = (XrBaseOutStructure *)properties->next;
	properties->next = &display_info_ext;

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
			DisplayXRState *state = displayxr_get_state();

			state->display_info.display_width_meters = di->displaySizeMeters.width;
			state->display_info.display_height_meters = di->displaySizeMeters.height;
			state->display_info.display_pixel_width = di->displayPixelWidth;
			state->display_info.display_pixel_height = di->displayPixelHeight;
			state->display_info.nominal_viewer_x = di->nominalViewerPositionInDisplaySpace.x;
			state->display_info.nominal_viewer_y = di->nominalViewerPositionInDisplaySpace.y;
			state->display_info.nominal_viewer_z = di->nominalViewerPositionInDisplaySpace.z;
			state->display_info.recommended_view_scale_x = di->recommendedViewScaleX;
			state->display_info.recommended_view_scale_y = di->recommendedViewScaleY;
			state->display_info.is_valid = 1;

			fprintf(stderr, "[DisplayXR] xrGetSystemProperties: display=%ux%u, %.3fx%.3fm\n",
			        di->displayPixelWidth, di->displayPixelHeight,
			        di->displaySizeMeters.width, di->displaySizeMeters.height);

#if defined(__APPLE__)
			// Create shared IOSurface now — before xrCreateSession reads it.
			// Only in editor mode (IOSurface for zero-copy preview). Built apps
			// render directly to the overlay CAMetalLayer, no IOSurface needed.
			if (state->editor_mode &&
			    state->shared_iosurface == nullptr &&
			    di->displayPixelWidth > 0 && di->displayPixelHeight > 0) {
				if (displayxr_metal_create_shared_surface(
				        di->displayPixelWidth, di->displayPixelHeight)) {
					fprintf(stderr, "[DisplayXR] Shared IOSurface created: %ux%u\n",
					        di->displayPixelWidth, di->displayPixelHeight);
				}
			}
#endif

			// Look up display mode function (always try — deprecated but still supported)
			if (s_next_gipa && s_instance) {
				PFN_xrVoidFunction fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrRequestDisplayModeEXT", &fn)) && fn) {
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
	DisplayXRState *state = displayxr_get_state();

	// Unity may call xrGetSystemProperties AFTER xrCreateSession, so we
	// force-call it here to ensure display info (and the IOSurface) are
	// populated before we inject the binding struct.
	if (!state->display_info.is_valid && s_real_get_system_properties != nullptr) {
		XrSystemProperties sys_props = {XR_TYPE_SYSTEM_PROPERTIES};
		hooked_xrGetSystemProperties(instance, createInfo->systemId, &sys_props);
		fprintf(stderr, "[DisplayXR] Force-called xrGetSystemProperties: is_valid=%d\n",
		        state->display_info.is_valid);
	}

	// Log the graphics binding type Unity is using
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
				fprintf(stderr, "[DisplayXR] Graphics binding: VULKAN (via MoltenVK on macOS)\n");
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
				fprintf(stderr, "[DisplayXR] Graphics binding: D3D11\n");
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
				fprintf(stderr, "[DisplayXR] Graphics binding: D3D12\n");
			} else {
				fprintf(stderr, "[DisplayXR] Session chain struct type=%u\n", (unsigned)item->type);
			}
			item = item->next;
		}
	}

	// Inject window binding into the next chain.
	{
#if defined(__APPLE__)
		// Create shared IOSurface if display info is available but surface wasn't created yet.
		// Only in editor mode (IOSurface for zero-copy preview). Built apps auto-detect
		// the window and render directly to the overlay CAMetalLayer.
		if (state->editor_mode &&
		    state->shared_iosurface == nullptr && state->display_info.is_valid &&
		    state->display_info.display_pixel_width > 0 &&
		    state->display_info.display_pixel_height > 0) {
			if (displayxr_metal_create_shared_surface(
			        state->display_info.display_pixel_width,
			        state->display_info.display_pixel_height)) {
				fprintf(stderr, "[DisplayXR] Shared IOSurface created in xrCreateSession: %ux%u\n",
				        state->display_info.display_pixel_width,
				        state->display_info.display_pixel_height);
			}
		}

		// Auto-detect the app's main window and create an overlay view/HWND.
		// Only for built apps (not editor mode) — the editor uses IOSurface
		// for zero-copy preview and doesn't need window auto-detection.
		if (state->window_handle == nullptr && !state->editor_mode) {
			void *view = displayxr_get_app_main_view();
			if (view != nullptr) {
				state->window_handle = view;
				fprintf(stderr, "[DisplayXR] Auto-detected main window (overlay): %p\n", view);
			} else {
				fprintf(stderr, "[DisplayXR] No main window found — offscreen mode\n");
			}
		}
#elif defined(_WIN32)
		// Auto-detect Unity's main HWND and create an overlay child window.
		// Only for built apps — editor uses shared texture mode.
		if (state->window_handle == nullptr && !state->editor_mode) {
			void *hwnd = displayxr_get_app_main_view();
			if (hwnd != nullptr) {
				state->window_handle = hwnd;
				fprintf(stderr, "[DisplayXR] Auto-detected main window HWND (overlay): %p\n", hwnd);
			} else {
				fprintf(stderr, "[DisplayXR] No main window HWND found — compositor will create own window\n");
			}
		}
#endif

		// Walk the chain to find the last item before NULL
		const XrBaseInStructure *chain = (const XrBaseInStructure *)createInfo->next;
		const XrBaseInStructure *last_in_chain = nullptr;

		while (chain != nullptr) {
			last_in_chain = chain;
			chain = chain->next;
		}

		if (last_in_chain != nullptr) {
#if defined(_WIN32)
			static XrWin32WindowBindingCreateInfoEXT win_binding = {};
			win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
			win_binding.next = nullptr;
			win_binding.windowHandle = state->window_handle;
			win_binding.readbackCallback = displayxr_readback_callback;
			win_binding.readbackUserdata = nullptr;
			win_binding.sharedTextureHandle = state->shared_d3d_handle;

			fprintf(stderr, "[DisplayXR] Injecting win32 window binding: windowHandle=%p, sharedTextureHandle=%p\n",
			        win_binding.windowHandle, win_binding.sharedTextureHandle);

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&win_binding;
#elif defined(__APPLE__)
			static XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
			mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
			mac_binding.next = nullptr;
			mac_binding.viewHandle = state->window_handle;
			mac_binding.readbackCallback = displayxr_readback_callback;
			mac_binding.readbackUserdata = nullptr;
			mac_binding.sharedIOSurface = state->shared_iosurface;

			fprintf(stderr, "[DisplayXR] Injecting cocoa window binding: viewHandle=%p, sharedIOSurface=%p\n",
			        mac_binding.viewHandle, mac_binding.sharedIOSurface);

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&mac_binding;
#endif
		}
	}

	XrResult result = s_real_create_session(instance, createInfo, session);
	if (XR_SUCCEEDED(result)) {
		s_session = *session;
		s_session_alive = 1;
		fprintf(stderr, "[DisplayXR] xrCreateSession succeeded, session=%p\n", (void *)(uintptr_t)s_session);

		// Create LOCAL reference space for xrLocateViews.
		// LOCAL space gives raw eye positions relative to the display origin.
		if (s_real_create_reference_space != nullptr) {
			XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			space_info.poseInReferenceSpace.orientation = {0, 0, 0, 1};
			space_info.poseInReferenceSpace.position = {0, 0, 0};

			XrResult space_result = s_real_create_reference_space(s_session, &space_info, &s_local_space);
			if (XR_FAILED(space_result)) {
				s_local_space = XR_NULL_HANDLE;
				fprintf(stderr, "[DisplayXR] LOCAL reference space FAILED (result=%d) — "
				        "will use app's reference space\n", space_result);
			} else {
				fprintf(stderr, "[DisplayXR] LOCAL reference space created successfully\n");
			}
		}
	}

	return result;
}

static XrResult XRAPI_CALL
hooked_xrDestroySession(XrSession session)
{
	fprintf(stderr, "[DisplayXR] xrDestroySession BEGIN session=%p (DEFERRED)\n", (void *)(uintptr_t)session);
	s_session_alive = 0;
	s_local_space = XR_NULL_HANDLE;

	// Defer the real destroy — Unity calls xrPollEvent after xrDestroyInstance,
	// and its dispatch trampolines reference runtime session/compositor objects.
	// Keep everything alive until the next instance lifecycle.
	s_deferred_destroy_session = session;
	s_deferred_destroy_session_fn = s_real_destroy_session;

	fprintf(stderr, "[DisplayXR] xrDestroySession END (deferred, returning XR_SUCCESS)\n");
	s_session = XR_NULL_HANDLE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
hooked_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
	// Guard: skip if session is being torn down
	if (!s_session_alive) {
		fprintf(stderr, "[DisplayXR] xrEndFrame: session not alive, passing through\n");
		return s_real_end_frame(session, frameEndInfo);
	}

	// Diagnostic: log what Unity submits (every 120 frames, skip first 2)
	static int s_ef_count = 0;
	if (s_ef_count >= 2 && s_ef_count % 120 == 0 &&
	    frameEndInfo != nullptr && frameEndInfo->layerCount > 0 &&
	    frameEndInfo->layers != nullptr) {
		fprintf(stderr, "[DisplayXR] xrEndFrame: %u layers\n", frameEndInfo->layerCount);
		for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
			const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
			if (hdr == nullptr) continue;
			if (hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
				const XrCompositionLayerProjection *proj =
				    (const XrCompositionLayerProjection *)hdr;
				if (proj->views == nullptr) continue;
				fprintf(stderr, "  layer[%u] PROJECTION: viewCount=%u\n",
				        i, proj->viewCount);
				for (uint32_t v = 0; v < proj->viewCount; v++) {
					const XrCompositionLayerProjectionView *pv = &proj->views[v];
					float hfov = (pv->fov.angleRight - pv->fov.angleLeft) * 57.2958f;
					fprintf(stderr,
					        "    view[%u]: pos=(%.4f,%.4f,%.4f) hfov=%.1f "
					        "arrayIdx=%u rect=(%d,%d %dx%d)\n",
					        v,
					        pv->pose.position.x, pv->pose.position.y,
					        pv->pose.position.z,
					        hfov,
					        pv->subImage.imageArrayIndex,
					        pv->subImage.imageRect.offset.x,
					        pv->subImage.imageRect.offset.y,
					        pv->subImage.imageRect.extent.width,
					        pv->subImage.imageRect.extent.height);
				}
			} else {
				fprintf(stderr, "  layer[%u] type=%u\n", i, (unsigned)hdr->type);
			}
		}
	}
	s_ef_count++;

	DisplayXRState *state = displayxr_get_state();

	// Count active window-space layers
	int active_layers = 0;
	for (int i = 0; i < DISPLAYXR_MAX_WINDOW_LAYERS; i++) {
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
	static XrCompositionLayerWindowSpaceEXT ws_layers[DISPLAYXR_MAX_WINDOW_LAYERS] = {};
	uint32_t idx = frameEndInfo->layerCount;
	for (int i = 0; i < DISPLAYXR_MAX_WINDOW_LAYERS; i++) {
		DisplayXRWindowLayer *wl = &state->window_layers[i];
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



static XrResult XRAPI_CALL
hooked_xrDestroyInstance(XrInstance instance)
{
	fprintf(stderr, "[DisplayXR] xrDestroyInstance BEGIN (DEFERRED)\n");
	s_instance_alive = 0;

	// Defer the real destroy — Unity's OpenXR loader calls xrPollEvent AFTER
	// xrDestroyInstance returns, through JIT-generated dispatch trampolines that
	// reference runtime memory (code pages, dispatch tables, session/compositor
	// objects). If we destroy now, those trampolines read freed pages → SIGSEGV.
	//
	// Instead, mark everything as dead (guards will reject API calls) but keep
	// the runtime instance alive. The actual destroy happens at the start of
	// the next instance lifecycle in displayxr_install_hooks().
	s_deferred_destroy_instance = instance;
	s_deferred_destroy_instance_fn = s_real_destroy_instance;

	// Null out function pointers so our guards reject post-destroy calls,
	// but the runtime's actual objects stay allocated and mapped.
	s_real_locate_views = nullptr;
	s_real_get_system_properties = nullptr;
	s_real_create_session = nullptr;
	s_real_destroy_session = nullptr;
	s_real_end_frame = nullptr;
	s_real_create_reference_space = nullptr;
	s_real_poll_event = nullptr;
	s_real_destroy_instance = nullptr;

	fprintf(stderr, "[DisplayXR] xrDestroyInstance END (deferred, returning XR_SUCCESS)\n");
	s_instance = XR_NULL_HANDLE;
	s_session = XR_NULL_HANDLE;
	s_local_space = XR_NULL_HANDLE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
hooked_xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData)
{
	// Load function pointer into local BEFORE any guards, so the compiler
	// can't reorder the load past the null check.
	PFN_xrPollEvent poll_fn = s_real_poll_event;

	// Guard 1: instance dead or function pointer nulled
	if (!s_instance_alive || poll_fn == nullptr) {
		return XR_EVENT_UNAVAILABLE;
	}

	// Guard 2: stop polling after EXITING event
	if (s_stop_polling) {
		return XR_EVENT_UNAVAILABLE;
	}

	XrResult result = poll_fn(instance, eventData);

	// After EXITING, null out the function pointer to prevent ALL future calls.
	// This is the nuclear guard: even if s_stop_polling is somehow reset,
	// the nullptr check in guard 1 will catch it.
	if (result == XR_SUCCESS && eventData != nullptr &&
	    eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
		const XrEventDataSessionStateChanged *ssc =
			(const XrEventDataSessionStateChanged *)eventData;
		if (ssc->state == XR_SESSION_STATE_EXITING ||
		    ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
			fprintf(stderr, "[DisplayXR] xrPollEvent: EXITING detected, nulling poll function\n");
			s_stop_polling = 1;
			s_real_poll_event = nullptr; // Nuclear: guard 1 catches all future calls
			s_instance_alive = 0;        // Belt and suspenders
		}
	}

	return result;
}


// ============================================================================
// Hook installation — called by Unity's OpenXR Feature
// ============================================================================

XrResult XRAPI_CALL
displayxr_hook_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function)
{
	// First get the real function from the next in chain
	XrResult result = s_next_gipa(instance, name, function);
	if (XR_FAILED(result)) {
		return result;
	}

	// Cache instance handle
	if (instance != XR_NULL_HANDLE) {
		s_instance = instance;
		s_instance_alive = 1;
	}

	// Pin the runtime library in memory so Unity's dlclose doesn't unmap
	// its code pages. We defer xrDestroySession/xrDestroyInstance to keep
	// runtime objects alive, but Unity calls Internal_UnloadOpenXRLibrary()
	// which dlcloses the runtime. RTLD_NODELETE prevents the unmap, so
	// post-destroy xrPollEvent calls (from editor repaint paths) can still
	// safely reach the runtime's dispatch stubs.
#if !defined(_WIN32)
	if (!s_runtime_pinned && *function != nullptr) {
		Dl_info dl_info;
		if (dladdr((void *)*function, &dl_info) && dl_info.dli_fname) {
			void *handle = dlopen(dl_info.dli_fname, RTLD_LAZY | RTLD_NODELETE);
			if (handle) {
				fprintf(stderr, "[DisplayXR] Pinned runtime library: %s\n", dl_info.dli_fname);
				s_runtime_pinned = 1;
				dlclose(handle); // Decrement refcount but RTLD_NODELETE keeps it mapped
			}
		}
	}
#endif

	// Log function resolution for debugging second-instance issues
	fprintf(stderr, "[DisplayXR] xrGetInstanceProcAddr: resolving '%s'\n", name);

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
	} else if (strcmp(name, "xrDestroySession") == 0) {
		s_real_destroy_session = (PFN_xrDestroySession)*function;
		*function = (PFN_xrVoidFunction)hooked_xrDestroySession;
	} else if (strcmp(name, "xrEndFrame") == 0) {
		s_real_end_frame = (PFN_xrEndFrame)*function;
		*function = (PFN_xrVoidFunction)hooked_xrEndFrame;
	} else if (strcmp(name, "xrCreateReferenceSpace") == 0) {
		s_real_create_reference_space = (PFN_xrCreateReferenceSpace)*function;
		// Don't intercept — just cache the function pointer
	} else if (strcmp(name, "xrPollEvent") == 0) {
		s_real_poll_event = (PFN_xrPollEvent)*function;
		*function = (PFN_xrVoidFunction)hooked_xrPollEvent;
	} else if (strcmp(name, "xrDestroyInstance") == 0) {
		s_real_destroy_instance = (PFN_xrDestroyInstance)*function;
		*function = (PFN_xrVoidFunction)hooked_xrDestroyInstance;
	}

	return result;
}

PFN_xrVoidFunction
displayxr_install_hooks(PFN_xrGetInstanceProcAddr next_gipa)
{
	fprintf(stderr, "[DisplayXR] install_hooks called (new instance lifecycle)\n");

	// Clear deferred session/instance destruction from the previous lifecycle.
	// These were deferred because Unity's loader calls xrPollEvent after
	// xrDestroyInstance through dispatch trampolines that reference runtime memory.
	// We do NOT call the saved destroy functions here — Unity has already called
	// Internal_UnloadOpenXRLibrary() (dlclose) between play sessions, so the
	// saved function pointers are dangling. The runtime cleans up via dlclose.
	if (s_deferred_destroy_session != XR_NULL_HANDLE) {
		fprintf(stderr, "[DisplayXR] Clearing deferred xrDestroySession (runtime was unloaded by Unity)\n");
		s_deferred_destroy_session = XR_NULL_HANDLE;
		s_deferred_destroy_session_fn = nullptr;
	}
	if (s_deferred_destroy_instance != XR_NULL_HANDLE) {
		fprintf(stderr, "[DisplayXR] Clearing deferred xrDestroyInstance (runtime was unloaded by Unity)\n");
		s_deferred_destroy_instance = XR_NULL_HANDLE;
		s_deferred_destroy_instance_fn = nullptr;
	}

	displayxr_state_init();
	s_next_gipa = next_gipa;

	// Reset all state for the new instance lifecycle.
	// Previous function pointers may be stale if the old instance was destroyed.
	s_real_locate_views = nullptr;
	s_real_get_system_properties = nullptr;
	s_real_create_session = nullptr;
	s_real_destroy_session = nullptr;
	s_real_end_frame = nullptr;
	s_real_create_reference_space = nullptr;
	s_real_poll_event = nullptr;
	s_real_destroy_instance = nullptr;
	s_instance = XR_NULL_HANDLE;
	s_session = XR_NULL_HANDLE;
	s_local_space = XR_NULL_HANDLE;
	s_session_alive = 0;
	s_instance_alive = 0;
	s_stop_polling = 0;

	return (PFN_xrVoidFunction)displayxr_hook_xrGetInstanceProcAddr;
}


// ============================================================================
// P/Invoke exports
// ============================================================================

void
displayxr_stop_polling(void)
{
	fprintf(stderr, "[DisplayXR] displayxr_stop_polling: killing poll forwarding\n");
	s_stop_polling = 1;
	s_real_poll_event = nullptr;
	s_instance_alive = 0;
	s_session_alive = 0;
}

void
displayxr_set_tunables(float ipd_factor,
                      float parallax_factor,
                      float perspective_factor,
                      float virtual_display_height,
                      float inv_convergence_distance,
                      float fov_override,
                      float near_z,
                      float far_z,
                      int camera_centric)
{
	DisplayXRTunables t;
	t.ipd_factor = ipd_factor;
	t.parallax_factor = parallax_factor;
	t.perspective_factor = perspective_factor;
	t.virtual_display_height = virtual_display_height;
	t.inv_convergence_distance = inv_convergence_distance;
	t.fov_override = fov_override;
	t.near_z = near_z > 0.0001f ? near_z : 0.01f;
	t.far_z = far_z > t.near_z ? far_z : 1000.0f;
	t.camera_centric = camera_centric ? 1 : 0;
	displayxr_state_set_tunables(&t);
}

void
displayxr_get_display_info(float *display_width_m,
                          float *display_height_m,
                          uint32_t *pixel_width,
                          uint32_t *pixel_height,
                          float *nominal_x,
                          float *nominal_y,
                          float *nominal_z,
                          float *scale_x,
                          float *scale_y,
                          int *is_valid)
{
	DisplayXRState *state = displayxr_get_state();
	DisplayXRDisplayInfo *di = &state->display_info;

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
displayxr_get_eye_positions(float *lx, float *ly, float *lz, float *rx, float *ry, float *rz, int *is_tracked)
{
	DisplayXREyePositions eyes = displayxr_state_get_eye_positions();
	*lx = eyes.left_eye.x;
	*ly = eyes.left_eye.y;
	*lz = eyes.left_eye.z;
	*rx = eyes.right_eye.x;
	*ry = eyes.right_eye.y;
	*rz = eyes.right_eye.z;
	*is_tracked = eyes.is_tracked;
}

void
displayxr_set_window_handle(void *handle)
{
	DisplayXRState *state = displayxr_get_state();
	state->window_handle = handle;
}

void
displayxr_set_editor_mode(int enabled)
{
	DisplayXRState *state = displayxr_get_state();
	state->editor_mode = (uint8_t)(enabled != 0);
}

// When set, the native WM_SIZE handler is the sole source of viewport
// dimensions.  C# LateUpdate calls (displayxr_set_viewport_size) become
// no-ops to avoid overwriting the correct values with stale Screen.width/height
// that lag one frame behind.
static int s_native_viewport_active = 0;

void
displayxr_set_viewport_size(uint32_t width, uint32_t height)
{
	if (s_native_viewport_active)
		return; // WM_SIZE is driving viewport — ignore C# push
	DisplayXRState *state = displayxr_get_state();
	state->viewport_width = width;
	state->viewport_height = height;
}

void
displayxr_set_viewport_size_native(uint32_t width, uint32_t height)
{
	s_native_viewport_active = 1;
	DisplayXRState *state = displayxr_get_state();
	state->viewport_width = width;
	state->viewport_height = height;
}

int
displayxr_request_display_mode(int mode_3d)
{
	DisplayXRState *state = displayxr_get_state();
	if (!state->has_display_mode_ext || state->pfn_request_display_mode == nullptr || s_session == XR_NULL_HANDLE) {
		return 0; // Not supported
	}

	XrDisplayModeEXT mode = mode_3d ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
	XrResult result = state->pfn_request_display_mode(s_session, mode);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

void
displayxr_set_scene_transform(float pos_x,
                             float pos_y,
                             float pos_z,
                             float ori_x,
                             float ori_y,
                             float ori_z,
                             float ori_w,
                             float scale_x,
                             float scale_y,
                             float scale_z,
                             int enabled)
{
	DisplayXRSceneTransform t;
	t.position[0] = pos_x;
	t.position[1] = pos_y;
	t.position[2] = pos_z;
	t.orientation[0] = ori_x;
	t.orientation[1] = ori_y;
	t.orientation[2] = ori_z;
	t.orientation[3] = ori_w;
	t.scale[0] = scale_x;
	t.scale[1] = scale_y;
	t.scale[2] = scale_z;
	t.enabled = enabled ? 1 : 0;
	displayxr_state_set_scene_transform(&t);
}

void
displayxr_get_stereo_matrices(float *left_view, float *left_proj,
                              float *right_view, float *right_proj,
                              int *valid)
{
	DisplayXRStereoMatrices mats = displayxr_state_get_stereo_matrices();
	memcpy(left_view, mats.left_view, sizeof(float) * 16);
	memcpy(left_proj, mats.left_projection, sizeof(float) * 16);
	memcpy(right_view, mats.right_view, sizeof(float) * 16);
	memcpy(right_proj, mats.right_projection, sizeof(float) * 16);
	*valid = mats.valid;
}

void
displayxr_get_readback(uint8_t **pixels, uint32_t *width, uint32_t *height, int *ready)
{
	DisplayXRState *state = displayxr_get_state();
	*pixels = state->readback_pixels;
	*width = state->readback_width;
	*height = state->readback_height;
	*ready = state->readback_ready;
}

void *
displayxr_create_shared_texture(uint32_t width, uint32_t height)
{
#if defined(__APPLE__)
	if (displayxr_metal_create_shared_surface(width, height)) {
		return displayxr_metal_get_texture();
	}
#endif
	// Windows: deferred to a later PR
	return nullptr;
}

void
displayxr_destroy_shared_texture(void)
{
#if defined(__APPLE__)
	displayxr_metal_destroy_shared_surface();
#endif
	DisplayXRState *state = displayxr_get_state();
	state->shared_iosurface = nullptr;
	state->shared_d3d_handle = nullptr;
	state->shared_texture_width = 0;
	state->shared_texture_height = 0;
	state->shared_texture_ready = 0;
}

void
displayxr_get_shared_texture(void **native_ptr, uint32_t *width, uint32_t *height, int *ready)
{
	DisplayXRState *state = displayxr_get_state();
#if defined(__APPLE__)
	*native_ptr = displayxr_metal_get_texture();
#elif defined(_WIN32)
	*native_ptr = state->shared_d3d_handle;
#else
	*native_ptr = nullptr;
#endif
	*width = state->shared_texture_width;
	*height = state->shared_texture_height;
	*ready = state->shared_texture_ready;
}
