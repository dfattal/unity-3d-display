// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenGL ES graphics backend for the DisplayXR hook chain (Android).
// Implements SBS composite via glCopyImageSubData (GLES 3.2 / GL_OES_copy_image).

#if defined(ENABLE_OPENGL) && defined(__ANDROID__)

#include "displayxr_hooks_internal.h"
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <dlfcn.h>

#define XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR ((XrStructureType)1000024001)
#define XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR           ((XrStructureType)1000024003)

typedef struct XrGraphicsBindingOpenGLESAndroidKHR {
	XrStructureType type;
	const void     *next;
	EGLDisplay      display;
	EGLConfig       config;
	EGLContext      context;
} XrGraphicsBindingOpenGLESAndroidKHR;

typedef struct XrSwapchainImageOpenGLESKHR {
	XrStructureType type;
	void           *next;
	uint32_t        image; // GL texture name
} XrSwapchainImageOpenGLESKHR;

// glCopyImageSubData — core in GLES 3.2, extension (GL_OES_copy_image) in older.
typedef void (*PFNGLCOPYIMAGESUBDATAPROC)(
	GLuint srcName, GLenum srcTarget, GLint srcLevel,
	GLint srcX, GLint srcY, GLint srcZ,
	GLuint dstName, GLenum dstTarget, GLint dstLevel,
	GLint dstX, GLint dstY, GLint dstZ,
	GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);

static PFNGLCOPYIMAGESUBDATAPROC s_glCopyImageSubData = nullptr;

// ---------------------------------------------------------------------------
// Per-swapchain tracking
// ---------------------------------------------------------------------------
struct GLESScSub {
	XrSwapchain xr_sc;
	uint32_t    width, height;
	uint32_t    images[8]; // GL texture names
	uint32_t    img_count;
	uint32_t    current_idx;
	bool        release_pending;
	bool        active;
};

// ---------------------------------------------------------------------------
// OpenGLESBackend
// ---------------------------------------------------------------------------
class OpenGLESBackend : public GraphicsBackend {
public:
	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;

	GLESScSub sc_subs[8] = {};
	int       sc_sub_count = 0;

	XrSwapchain sbs_sc = XR_NULL_HANDLE;
	uint32_t    sbs_images[8] = {};
	uint32_t    sbs_img_count = 0;

private:
	GLESScSub *sub_find(XrSwapchain sc)
	{
		for (int i = 0; i < sc_sub_count; i++) {
			if (sc_subs[i].active && sc_subs[i].xr_sc == sc)
				return &sc_subs[i];
		}
		return nullptr;
	}

	void sub_cleanup_all()
	{
		sc_sub_count = 0;
		for (int i = 0; i < 8; i++) {
			sc_subs[i].active = false;
			sc_subs[i].xr_sc  = XR_NULL_HANDLE;
		}
		if (sbs_sc != XR_NULL_HANDLE && s_real_destroy_swapchain) {
			s_real_destroy_swapchain(sbs_sc);
			sbs_sc = XR_NULL_HANDLE;
		}
		sbs_img_count = 0;
	}

	bool make_context_current()
	{
		if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT)
			return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context) == EGL_TRUE;
		return false;
	}

	void load_extensions()
	{
		if (s_glCopyImageSubData) return;
		// Try core GLES 3.2 first, fall back to OES extension
		s_glCopyImageSubData = (PFNGLCOPYIMAGESUBDATAPROC)eglGetProcAddress("glCopyImageSubData");
		if (!s_glCopyImageSubData)
			s_glCopyImageSubData = (PFNGLCOPYIMAGESUBDATAPROC)eglGetProcAddress("glCopyImageSubDataOES");
		if (!s_glCopyImageSubData)
			displayxr_log("[DisplayXR] OpenGLES: glCopyImageSubData not available\n");
	}

public:
	void on_session_created(const XrSessionCreateInfo *createInfo) override
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR) {
				const XrGraphicsBindingOpenGLESAndroidKHR *binding =
				    (const XrGraphicsBindingOpenGLESAndroidKHR *)item;
				egl_display = binding->display;
				egl_context = binding->context;
				displayxr_log("[DisplayXR] OpenGLES: EGLDisplay=%p EGLContext=%p\n",
				              (void *)egl_display, (void *)egl_context);
				break;
			}
			item = item->next;
		}
	}

	void on_session_destroyed() override { sub_cleanup_all(); }
	void on_destroy() override
	{
		sub_cleanup_all();
		egl_display = EGL_NO_DISPLAY;
		egl_context = EGL_NO_CONTEXT;
	}

	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
	{
		// No window binding extension for Android/OpenGL ES
		(void)last; (void)state;
	}

	void on_swapchain_created(XrSession /*session*/, const XrSwapchainCreateInfo *createInfo,
	                           XrSwapchain unity_sc) override
	{
		if ((createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
		    sc_sub_count < 8) {
			GLESScSub &sub  = sc_subs[sc_sub_count++];
			sub.xr_sc       = unity_sc;
			sub.width       = createInfo->width;
			sub.height      = createInfo->height;
			sub.img_count   = 0;
			sub.current_idx = 0;
			sub.release_pending = false;
			sub.active      = true;
		}
	}

	bool handle_enumerate_swapchain_images(XrSwapchain swapchain, uint32_t imageCapacityInput,
	                                        uint32_t *imageCountOutput,
	                                        XrSwapchainImageBaseHeader *images,
	                                        XrResult *result_out) override
	{
		GLESScSub *sub = sub_find(swapchain);
		if (!sub) return false;

		if (images != nullptr && images->type == XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) {
			XrResult result = s_real_enumerate_swapchain_images(
			    swapchain, imageCapacityInput, imageCountOutput, images);
			if (XR_SUCCEEDED(result) && imageCountOutput) {
				XrSwapchainImageOpenGLESKHR *gl_imgs = (XrSwapchainImageOpenGLESKHR *)images;
				uint32_t n = (*imageCountOutput < 8) ? *imageCountOutput : 8;
				sub->img_count = n;
				for (uint32_t i = 0; i < n; i++)
					sub->images[i] = gl_imgs[i].image;
			}
			*result_out = result;
			return true;
		}
		*result_out = s_real_enumerate_swapchain_images(
		    swapchain, imageCapacityInput, imageCountOutput, images);
		return true;
	}

	bool handle_acquire_swapchain_image(XrSwapchain swapchain,
	                                     const XrSwapchainImageAcquireInfo *acquireInfo,
	                                     uint32_t *index, XrResult *result_out) override
	{
		GLESScSub *sub = sub_find(swapchain);
		if (!sub) return false;
		XrResult result = s_real_acquire_swapchain_image(swapchain, acquireInfo, index);
		if (XR_SUCCEEDED(result) && index)
			sub->current_idx = *index;
		*result_out = result;
		return true;
	}

	bool handle_wait_swapchain_image(XrSwapchain swapchain,
	                                  const XrSwapchainImageWaitInfo *waitInfo,
	                                  XrResult *result_out) override
	{
		GLESScSub *sub = sub_find(swapchain);
		if (!sub) return false;
		*result_out = s_real_wait_swapchain_image(swapchain, waitInfo);
		return true;
	}

	bool handle_release_swapchain_image(XrSwapchain swapchain,
	                                     const XrSwapchainImageReleaseInfo *releaseInfo,
	                                     XrResult *result_out) override
	{
		GLESScSub *sub = sub_find(swapchain);
		if (!sub) return false;
		// Defer release until after SBS composite in prepare_end_frame.
		sub->release_pending = true;
		(void)releaseInfo;
		*result_out = XR_SUCCESS;
		return true;
	}

	void prepare_end_frame(XrSession session, const XrFrameEndInfo *frameEndInfo,
	                        void *patches_out, int *npatch_out) override
	{
		EFPatch *ef_patches = (EFPatch *)patches_out;
		*npatch_out = 0;

		if (!frameEndInfo || !frameEndInfo->layers) goto flush_deferred;

		for (uint32_t i = 0; i < frameEndInfo->layerCount && *npatch_out < 8; i++) {
			const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
			if (!hdr || hdr->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
			const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection *)hdr;
			if (!proj->views || proj->viewCount != 2) continue;
			XrCompositionLayerProjectionView *views = (XrCompositionLayerProjectionView *)proj->views;

			GLESScSub *sub1 = sub_find(views[0].subImage.swapchain);
			GLESScSub *sub2 = sub_find(views[1].subImage.swapchain);
			if (!sub1 || !sub2 || sub1 == sub2) continue;

			uint32_t eye_w = sub1->width;
			uint32_t eye_h = sub1->height;
			uint32_t sbs_w = eye_w * 2;

			// Lazily create SBS swapchain
			if (sbs_sc == XR_NULL_HANDLE && s_real_create_swapchain) {
				XrSwapchainCreateInfo sbs_info;
				sbs_info.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
				sbs_info.next        = nullptr;
				sbs_info.createFlags = 0;
				sbs_info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
				                       XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
				sbs_info.format      = GL_RGBA8;
				sbs_info.sampleCount = 1;
				sbs_info.width       = sbs_w;
				sbs_info.height      = eye_h;
				sbs_info.faceCount   = 1;
				sbs_info.arraySize   = 1;
				sbs_info.mipCount    = 1;
				XrResult sr = s_real_create_swapchain(session, &sbs_info, &sbs_sc);
				if (XR_SUCCEEDED(sr) && s_real_enumerate_swapchain_images) {
					uint32_t cnt = 0;
					s_real_enumerate_swapchain_images(sbs_sc, 0, &cnt, nullptr);
					if (cnt > 0 && cnt <= 8) {
						XrSwapchainImageOpenGLESKHR imgs[8];
						for (uint32_t k = 0; k < cnt; k++) {
							imgs[k].type  = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
							imgs[k].next  = nullptr;
							imgs[k].image = 0;
						}
						s_real_enumerate_swapchain_images(
						    sbs_sc, cnt, &cnt,
						    (XrSwapchainImageBaseHeader *)imgs);
						sbs_img_count = cnt;
						for (uint32_t k = 0; k < cnt; k++)
							sbs_images[k] = imgs[k].image;
					}
					displayxr_log("[DisplayXR] OpenGLES SBS swapchain created: %ux%u imgs=%u\n",
					              sbs_w, eye_h, sbs_img_count);
				} else {
					displayxr_log("[DisplayXR] OpenGLES SBS swapchain creation FAILED: %d\n", (int)sr);
				}
			}
			if (sbs_sc == XR_NULL_HANDLE) continue;

			// Acquire + wait SBS image
			uint32_t sbs_idx = 0;
			XrSwapchainImageAcquireInfo acq;
			acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
			acq.next = nullptr;
			XrSwapchainImageWaitInfo wai;
			wai.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
			wai.next    = nullptr;
			wai.timeout = XR_INFINITE_DURATION;
			s_real_acquire_swapchain_image(sbs_sc, &acq, &sbs_idx);
			s_real_wait_swapchain_image(sbs_sc, &wai);

			uint32_t left_tex  = (sub1->current_idx < sub1->img_count) ? sub1->images[sub1->current_idx] : 0;
			uint32_t right_tex = (sub2->current_idx < sub2->img_count) ? sub2->images[sub2->current_idx] : 0;
			uint32_t sbs_tex   = (sbs_idx < sbs_img_count) ? sbs_images[sbs_idx] : 0;

			if (left_tex && right_tex && sbs_tex) {
				if (make_context_current()) {
					load_extensions();
					glFlush();

					if (s_glCopyImageSubData) {
						s_glCopyImageSubData(
						    left_tex,  GL_TEXTURE_2D, 0, 0, 0, 0,
						    sbs_tex,   GL_TEXTURE_2D, 0, 0, 0, 0,
						    (GLsizei)eye_w, (GLsizei)eye_h, 1);
						s_glCopyImageSubData(
						    right_tex, GL_TEXTURE_2D, 0, 0, 0, 0,
						    sbs_tex,  GL_TEXTURE_2D, 0, (GLint)eye_w, 0, 0,
						    (GLsizei)eye_w, (GLsizei)eye_h, 1);
						glFinish();
					} else {
						displayxr_log("[DisplayXR] OpenGLES: skipping SBS composite (glCopyImageSubData not loaded)\n");
					}
				}
			}

			// Release deferred swapchains
			XrSwapchainImageReleaseInfo rel;
			rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
			rel.next = nullptr;
			for (int si = 0; si < sc_sub_count; si++) {
				if (sc_subs[si].active && sc_subs[si].release_pending) {
					s_real_release_swapchain_image(sc_subs[si].xr_sc, &rel);
					sc_subs[si].release_pending = false;
				}
			}
			s_real_release_swapchain_image(sbs_sc, &rel);

			// Patch both views to reference sbs_sc
			for (uint32_t v = 0; v < 2 && *npatch_out < 8; v++) {
				EFPatch &p  = ef_patches[(*npatch_out)++];
				p.view      = &views[v];
				p.orig_sc   = views[v].subImage.swapchain;
				p.orig_rect = views[v].subImage.imageRect;
				views[v].subImage.swapchain              = sbs_sc;
				views[v].subImage.imageRect.offset.x     = (int32_t)(v * eye_w);
				views[v].subImage.imageRect.offset.y     = 0;
				views[v].subImage.imageRect.extent.width  = (int32_t)eye_w;
				views[v].subImage.imageRect.extent.height = (int32_t)eye_h;
			}

			static int s_sub_log = 0;
			if (s_sub_log++ < 4) {
				displayxr_log("[DisplayXR] OpenGLES xrEndFrame: SBS composite OK (eye=%ux%u sbs=%ux%u)\n",
				              eye_w, eye_h, sbs_w, eye_h);
			}
		}

	flush_deferred:
		XrSwapchainImageReleaseInfo rel;
		rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
		rel.next = nullptr;
		for (int si = 0; si < sc_sub_count; si++) {
			if (sc_subs[si].active && sc_subs[si].release_pending) {
				s_real_release_swapchain_image(sc_subs[si].xr_sc, &rel);
				sc_subs[si].release_pending = false;
			}
		}
	}

	void restore_end_frame(void *patches, int npatch) override
	{
		EFPatch *ef_patches = (EFPatch *)patches;
		for (int i = 0; i < npatch; i++) {
			ef_patches[i].view->subImage.swapchain = ef_patches[i].orig_sc;
			ef_patches[i].view->subImage.imageRect = ef_patches[i].orig_rect;
		}
	}

	void *create_shared_texture(uint32_t /*width*/, uint32_t /*height*/) override { return nullptr; }
	void  destroy_shared_texture() override {}
	void *get_shared_texture_native_ptr() override { return nullptr; }
};

GraphicsBackend *create_opengles_backend() { return new OpenGLESBackend(); }

#endif // defined(ENABLE_OPENGL) && defined(__ANDROID__)
