// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenGL ES standalone graphics backend stub (Android).
// Active when ENABLE_OPENGL is defined on Android.
// No editor preview window on Android — all methods return false/nullptr.

#if defined(ENABLE_OPENGL) && defined(__ANDROID__)

#include "displayxr_standalone_internal.h"
#include <android/log.h>

#define LOG_TAG "DisplayXR-SA-GLES"
#define SA_GLES_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

class StandaloneOpenGLESBackend : public StandaloneGraphicsBackend {
public:
	bool create_device(XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr) override
	{
		SA_GLES_LOG("StandaloneOpenGLESBackend: not implemented for Android (no editor)");
		return false;
	}

	bool create_shared_texture(uint32_t, uint32_t) override { return false; }
	void destroy_shared_texture() override {}
	void *get_shared_texture_native_ptr() override { return nullptr; }

	const void *build_session_binding(void *, void *) override
	{
		SA_GLES_LOG("StandaloneOpenGLESBackend: build_session_binding not implemented");
		return nullptr;
	}

	bool enumerate_atlas_images(XrSwapchain, PFN_xrEnumerateSwapchainImages, uint32_t) override { return false; }
	void *get_atlas_image(uint32_t) override { return nullptr; }
	void create_atlas_bridge(uint32_t, uint32_t, void *) override {}
	void destroy_atlas_bridge() override {}
	void *get_atlas_bridge_unity_ptr() override { return nullptr; }
	void blit_atlas(void *, uint32_t) override {}

	bool fw_create_swapchain(void *, uint32_t, uint32_t) override { return false; }
	void fw_destroy_swapchain() override {}
	void fw_resize_swapchain_buffers(uint32_t, uint32_t) override {}
	void fw_present(uint32_t, uint32_t) override {}

	void destroy() override {}
};

StandaloneGraphicsBackend *create_standalone_opengles_backend() { return new StandaloneOpenGLESBackend(); }

#endif // defined(ENABLE_OPENGL) && defined(__ANDROID__)
