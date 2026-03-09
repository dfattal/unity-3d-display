// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Standalone OpenXR session for editor preview.
// Loads the DisplayXR runtime directly via xrNegotiateLoaderRuntimeInterface,
// bypassing Unity's OpenXR loader entirely. This gives us full control over
// the session lifecycle — no deferred destruction, no signal handlers, no
// teardown races with Unity's Game View repaint cycle.

#include "displayxr_standalone.h"
#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"

#include <openxr/openxr.h>

#if defined(__APPLE__)
#include "displayxr_standalone_metal.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

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
// XR_KHR_metal_enable types (from openxr_platform.h, defined inline to avoid
// platform header dependency on the Khronos-fetched headers).
// ============================================================================

#define XR_TYPE_GRAPHICS_BINDING_METAL_KHR ((XrStructureType)1000029000)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR ((XrStructureType)1000029002)
#define XR_KHR_METAL_ENABLE_EXTENSION_NAME "XR_KHR_metal_enable"

typedef struct XrGraphicsBindingMetalKHR {
	XrStructureType type;
	const void *next;
	void *commandQueue;
} XrGraphicsBindingMetalKHR;

typedef struct XrGraphicsRequirementsMetalKHR {
	XrStructureType type;
	void *next;
	void *metalDevice;
} XrGraphicsRequirementsMetalKHR;

typedef XrResult (XRAPI_PTR *PFN_xrGetMetalGraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsMetalKHR *graphicsRequirements);


// ============================================================================
// Standalone session state
// ============================================================================

typedef struct StandaloneState {
	void *runtime_lib;
	PFN_xrGetInstanceProcAddr gipa;

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

	uint32_t tex_width;
	uint32_t tex_height;
	volatile int tex_ready;

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
} StandaloneState;

static StandaloneState s_sa = {};

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
	return 1;
}


// ============================================================================
// Public API
// ============================================================================

int
displayxr_standalone_start(const char *runtime_json_path)
{
	if (s_sa.running) {
		fprintf(stderr, "[DisplayXR-SA] Already running\n");
		return 1;
	}

	memset(&s_sa, 0, sizeof(s_sa));

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

#if defined(_WIN32)
	// TODO: Windows LoadLibrary path
	free(lib_abs);
	return 0;
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

	XrResult result = negotiate(&loader_info, &runtime_req);
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
	const char *extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
#if defined(__APPLE__)
		XR_KHR_METAL_ENABLE_EXTENSION_NAME,
		XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME,
#elif defined(_WIN32)
		XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME,
#endif
	};
	uint32_t ext_count = sizeof(extensions) / sizeof(extensions[0]);

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
		s_sa.display_info.supports_display_mode_switch = display_info_ext.supportsDisplayModeSwitch ? 1 : 0;
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
	space_ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	space_ci.poseInReferenceSpace.position = {0, 0, 0};

	result = s_sa.pfn_create_reference_space(s_sa.session, &space_ci, &s_sa.local_space);
	if (XR_FAILED(result)) {
		fprintf(stderr, "[DisplayXR-SA] xrCreateReferenceSpace failed: %d\n", result);
		s_sa.local_space = XR_NULL_HANDLE;
	}

	s_sa.running = 1;
	fprintf(stderr, "[DisplayXR-SA] Standalone session started successfully\n");
	return 1;
}


void
displayxr_standalone_stop(void)
{
	fprintf(stderr, "[DisplayXR-SA] Stopping standalone session\n");
	s_sa.running = 0;
	s_sa.session_ready = 0;

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
#endif

	if (s_sa.instance != XR_NULL_HANDLE && s_sa.pfn_destroy_instance) {
		s_sa.pfn_destroy_instance(s_sa.instance);
		s_sa.instance = XR_NULL_HANDLE;
		fprintf(stderr, "[DisplayXR-SA] Instance destroyed\n");
	}

#if !defined(_WIN32)
	// NOTE: Intentionally skip dlclose. The runtime may have background
	// dispatch queues (CVDisplayLink, GCD timers) that reference code in the
	// shared library. Unloading while those are still draining causes a
	// deferred SIGSEGV. Leaking the handle is harmless — the editor process
	// will reclaim it on exit, and we can re-dlopen on next Start().
	if (s_sa.runtime_lib) {
		// dlclose(s_sa.runtime_lib);  — see comment above
		s_sa.runtime_lib = NULL;
	}
#endif

	memset(&s_sa, 0, sizeof(s_sa));
	fprintf(stderr, "[DisplayXR-SA] Standalone session stopped\n");
}


int
displayxr_standalone_is_running(void)
{
	return s_sa.running;
}


void
displayxr_standalone_poll(void)
{
	if (!s_sa.running || !s_sa.pfn_poll_event) return;

	// --- Poll events ---
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

	// --- Frame loop (only when session is ready) ---
	if (!s_sa.session_ready) return;

	XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
	XrResult result = s_sa.pfn_wait_frame(s_sa.session, NULL, &frame_state);
	if (XR_FAILED(result)) return;

	result = s_sa.pfn_begin_frame(s_sa.session, NULL);
	if (XR_FAILED(result)) return;

	// --- Locate views (eye tracking) ---
	if (s_sa.local_space != XR_NULL_HANDLE && s_sa.pfn_locate_views) {
		XrViewLocateInfo locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = s_sa.local_space;

		XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		XrViewState view_state = {XR_TYPE_VIEW_STATE};
		uint32_t view_count = 0;

		result = s_sa.pfn_locate_views(s_sa.session, &locate_info,
		                               &view_state, 2, &view_count, views);
		if (XR_SUCCEEDED(result) && view_count >= 2) {
			s_sa.left_eye[0] = views[0].pose.position.x;
			s_sa.left_eye[1] = views[0].pose.position.y;
			s_sa.left_eye[2] = views[0].pose.position.z;
			s_sa.right_eye[0] = views[1].pose.position.x;
			s_sa.right_eye[1] = views[1].pose.position.y;
			s_sa.right_eye[2] = views[1].pose.position.z;
			s_sa.is_tracked = (view_state.viewStateFlags &
			                   XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
		}
	}

	// --- End frame (submit empty — keeps session alive for compositing) ---
	XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = 0;
	end_info.layers = NULL;

	s_sa.pfn_end_frame(s_sa.session, &end_info);
}


void
displayxr_standalone_get_display_info(float *display_width_m, float *display_height_m,
                                       uint32_t *pixel_width, uint32_t *pixel_height,
                                       float *nominal_x, float *nominal_y, float *nominal_z,
                                       float *scale_x, float *scale_y,
                                       int *supports_mode_switch, int *is_valid)
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
	*supports_mode_switch = di->supports_display_mode_switch;
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
displayxr_standalone_get_shared_texture(void **native_ptr, uint32_t *width,
                                         uint32_t *height, int *ready)
{
#if defined(__APPLE__)
	*native_ptr = displayxr_sa_metal_get_texture();
#else
	*native_ptr = NULL;
#endif
	*width = s_sa.tex_width;
	*height = s_sa.tex_height;
	*ready = s_sa.tex_ready;
}
