// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Vulkan graphics backend for the DisplayXR hook chain.
// Implements SBS composite via vkCmdCopyImage with image layout transitions.

#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))

#include "displayxr_hooks_internal.h"
#include <vulkan/vulkan.h>

// ---------------------------------------------------------------------------
// OpenXR Vulkan extension type IDs (inline — avoids XR_USE_GRAPHICS_API_VULKAN)
// ---------------------------------------------------------------------------
#define XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR      ((XrStructureType)1000090001)
#define XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR        ((XrStructureType)1000090002)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR  ((XrStructureType)1000090003)

typedef struct XrGraphicsBindingVulkanKHR {
	XrStructureType  type;
	const void      *next;
	VkInstance       instance;
	VkPhysicalDevice physicalDevice;
	VkDevice         device;
	uint32_t         queueFamilyIndex;
	uint32_t         queueIndex;
} XrGraphicsBindingVulkanKHR;

typedef struct XrSwapchainImageVulkanKHR {
	XrStructureType type;
	void           *next;
	VkImage         image;
} XrSwapchainImageVulkanKHR;

// ---------------------------------------------------------------------------
// Per-swapchain tracking
// ---------------------------------------------------------------------------
struct VkScSub {
	XrSwapchain xr_sc;
	uint32_t    width, height;
	VkImage     images[8];
	uint32_t    img_count;
	uint32_t    current_idx;
	bool        release_pending;
	bool        active;
};

// ---------------------------------------------------------------------------
// Image layout transition helper
// ---------------------------------------------------------------------------
static void transition_image_layout(VkCommandBuffer cb, VkImage image,
	VkImageLayout old_layout, VkImageLayout new_layout,
	VkAccessFlags src_access, VkAccessFlags dst_access,
	VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier barrier;
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.pNext = nullptr;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;
	vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------
// VulkanBackend
// ---------------------------------------------------------------------------
class VulkanBackend : public GraphicsBackend {
public:
	VkInstance       instance        = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice         device          = VK_NULL_HANDLE;
	uint32_t         queue_family_index = 0;
	VkQueue          queue           = VK_NULL_HANDLE;
	VkCommandPool    cmd_pool        = VK_NULL_HANDLE;
	VkCommandBuffer  cmd_buf         = VK_NULL_HANDLE;
	VkFence          fence           = VK_NULL_HANDLE;

	VkScSub  sc_subs[8] = {};
	int      sc_sub_count = 0;

	XrSwapchain sbs_sc = XR_NULL_HANDLE;
	VkImage     sbs_images[8] = {};
	uint32_t    sbs_img_count = 0;

private:
	VkScSub *sub_find(XrSwapchain sc)
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
			sc_subs[i].xr_sc = XR_NULL_HANDLE;
		}
		if (sbs_sc != XR_NULL_HANDLE && s_real_destroy_swapchain) {
			s_real_destroy_swapchain(sbs_sc);
			sbs_sc = XR_NULL_HANDLE;
		}
		sbs_img_count = 0;
	}

public:
	void on_session_created(const XrSessionCreateInfo *createInfo) override
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
				const XrGraphicsBindingVulkanKHR *binding = (const XrGraphicsBindingVulkanKHR *)item;
				instance         = binding->instance;
				physical_device  = binding->physicalDevice;
				device           = binding->device;
				queue_family_index = binding->queueFamilyIndex;

				vkGetDeviceQueue(device, queue_family_index, binding->queueIndex, &queue);

				VkCommandPoolCreateInfo pool_info;
				pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
				pool_info.pNext = nullptr;
				pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
				pool_info.queueFamilyIndex = queue_family_index;
				vkCreateCommandPool(device, &pool_info, nullptr, &cmd_pool);

				VkCommandBufferAllocateInfo alloc_info;
				alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
				alloc_info.pNext = nullptr;
				alloc_info.commandPool = cmd_pool;
				alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				alloc_info.commandBufferCount = 1;
				vkAllocateCommandBuffers(device, &alloc_info, &cmd_buf);

				VkFenceCreateInfo fence_info;
				fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
				fence_info.pNext = nullptr;
				fence_info.flags = 0;
				vkCreateFence(device, &fence_info, nullptr, &fence);

				displayxr_log("[DisplayXR] Vulkan backend: device=%p queueFamily=%u\n",
				              (void *)device, queue_family_index);
				break;
			}
			item = item->next;
		}
	}

	void on_session_destroyed() override
	{
		sub_cleanup_all();
	}

	void on_destroy() override
	{
		sub_cleanup_all();
		if (fence    != VK_NULL_HANDLE && device) { vkDestroyFence(device, fence, nullptr);           fence    = VK_NULL_HANDLE; }
		if (cmd_pool != VK_NULL_HANDLE && device) { vkDestroyCommandPool(device, cmd_pool, nullptr);  cmd_pool = VK_NULL_HANDLE; }
		cmd_buf = VK_NULL_HANDLE; // freed with pool
		device          = VK_NULL_HANDLE; // Not owned
		physical_device = VK_NULL_HANDLE;
		instance        = VK_NULL_HANDLE;
	}

	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
	{
#if defined(_WIN32)
		win32_inject_window_binding(last, state);
#else
		(void)last; (void)state; // No window binding extension for Vulkan on Linux/Android yet
#endif
	}

	void on_swapchain_created(XrSession /*session*/, const XrSwapchainCreateInfo *createInfo, XrSwapchain unity_sc) override
	{
		if ((createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
		    sc_sub_count < 8) {
			VkScSub &sub    = sc_subs[sc_sub_count++];
			sub.xr_sc       = unity_sc;
			sub.width       = createInfo->width;
			sub.height      = createInfo->height;
			sub.img_count   = 0;
			sub.current_idx = 0;
			sub.release_pending = false;
			sub.active      = true;
			displayxr_log("[DisplayXR] Vulkan: tracking swapchain %p (%ux%u)\n",
			              (void *)(uintptr_t)unity_sc, sub.width, sub.height);
		}
	}

	bool handle_enumerate_swapchain_images(XrSwapchain swapchain, uint32_t imageCapacityInput,
	                                        uint32_t *imageCountOutput,
	                                        XrSwapchainImageBaseHeader *images,
	                                        XrResult *result_out) override
	{
		VkScSub *sub = sub_find(swapchain);
		if (!sub) return false;

		if (images != nullptr && images->type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
			XrResult result = s_real_enumerate_swapchain_images(
			    swapchain, imageCapacityInput, imageCountOutput, images);
			if (XR_SUCCEEDED(result) && imageCountOutput) {
				XrSwapchainImageVulkanKHR *vk_imgs = (XrSwapchainImageVulkanKHR *)images;
				uint32_t n = (*imageCountOutput < 8) ? *imageCountOutput : 8;
				sub->img_count = n;
				for (uint32_t i = 0; i < n; i++)
					sub->images[i] = vk_imgs[i].image;
				displayxr_log("[DisplayXR] Vulkan: enumerated %u images for sc=%p\n", n,
				              (void *)(uintptr_t)swapchain);
			}
			*result_out = result;
			return true;
		}
		// Pass through with count query
		*result_out = s_real_enumerate_swapchain_images(
		    swapchain, imageCapacityInput, imageCountOutput, images);
		return true;
	}

	bool handle_acquire_swapchain_image(XrSwapchain swapchain,
	                                     const XrSwapchainImageAcquireInfo *acquireInfo,
	                                     uint32_t *index, XrResult *result_out) override
	{
		VkScSub *sub = sub_find(swapchain);
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
		VkScSub *sub = sub_find(swapchain);
		if (!sub) return false;
		*result_out = s_real_wait_swapchain_image(swapchain, waitInfo);
		return true;
	}

	bool handle_release_swapchain_image(XrSwapchain swapchain,
	                                     const XrSwapchainImageReleaseInfo *releaseInfo,
	                                     XrResult *result_out) override
	{
		VkScSub *sub = sub_find(swapchain);
		if (!sub) return false;
		// Defer release: we still need to read the image in prepare_end_frame.
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

		if (device == VK_NULL_HANDLE) goto flush_deferred;
		if (!frameEndInfo || !frameEndInfo->layers) goto flush_deferred;

		for (uint32_t i = 0; i < frameEndInfo->layerCount && *npatch_out < 8; i++) {
			const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
			if (!hdr || hdr->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
			const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection *)hdr;
			if (!proj->views || proj->viewCount != 2) continue;
			XrCompositionLayerProjectionView *views = (XrCompositionLayerProjectionView *)proj->views;

			VkScSub *sub1 = sub_find(views[0].subImage.swapchain);
			VkScSub *sub2 = sub_find(views[1].subImage.swapchain);
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
				sbs_info.format      = 37; // VK_FORMAT_R8G8B8A8_UNORM
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
						XrSwapchainImageVulkanKHR imgs[8];
						for (uint32_t k = 0; k < cnt; k++) {
							imgs[k].type  = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
							imgs[k].next  = nullptr;
							imgs[k].image = VK_NULL_HANDLE;
						}
						s_real_enumerate_swapchain_images(
						    sbs_sc, cnt, &cnt,
						    (XrSwapchainImageBaseHeader *)imgs);
						sbs_img_count = cnt;
						for (uint32_t k = 0; k < cnt; k++)
							sbs_images[k] = imgs[k].image;
					}
					displayxr_log("[DisplayXR] Vulkan SBS swapchain created: %ux%u imgs=%u\n",
					              sbs_w, eye_h, sbs_img_count);
				} else {
					displayxr_log("[DisplayXR] Vulkan SBS swapchain creation FAILED: %d\n", (int)sr);
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

			VkImage left_image  = (sub1->current_idx < sub1->img_count) ? sub1->images[sub1->current_idx] : VK_NULL_HANDLE;
			VkImage right_image = (sub2->current_idx < sub2->img_count) ? sub2->images[sub2->current_idx] : VK_NULL_HANDLE;
			VkImage sbs_image   = (sbs_idx < sbs_img_count) ? sbs_images[sbs_idx] : VK_NULL_HANDLE;

			if (left_image != VK_NULL_HANDLE && right_image != VK_NULL_HANDLE &&
			    sbs_image != VK_NULL_HANDLE && cmd_buf != VK_NULL_HANDLE) {
				// Wait for Unity rendering to finish
				vkQueueWaitIdle(queue);

				// Reset and begin command buffer
				vkResetCommandBuffer(cmd_buf, 0);
				VkCommandBufferBeginInfo begin_info;
				begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				begin_info.pNext            = nullptr;
				begin_info.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				begin_info.pInheritanceInfo = nullptr;
				vkBeginCommandBuffer(cmd_buf, &begin_info);

				// Transition left and right images to TRANSFER_SRC
				transition_image_layout(cmd_buf, left_image,
				    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				transition_image_layout(cmd_buf, right_image,
				    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				// Transition sbs image to TRANSFER_DST
				transition_image_layout(cmd_buf, sbs_image,
				    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    0, VK_ACCESS_TRANSFER_WRITE_BIT,
				    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

				// Copy left eye → left half of SBS
				VkImageCopy copy_left;
				copy_left.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
				copy_left.srcSubresource.mipLevel       = 0;
				copy_left.srcSubresource.baseArrayLayer = 0;
				copy_left.srcSubresource.layerCount     = 1;
				copy_left.srcOffset.x = 0; copy_left.srcOffset.y = 0; copy_left.srcOffset.z = 0;
				copy_left.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
				copy_left.dstSubresource.mipLevel       = 0;
				copy_left.dstSubresource.baseArrayLayer = 0;
				copy_left.dstSubresource.layerCount     = 1;
				copy_left.dstOffset.x = 0; copy_left.dstOffset.y = 0; copy_left.dstOffset.z = 0;
				copy_left.extent.width  = eye_w;
				copy_left.extent.height = eye_h;
				copy_left.extent.depth  = 1;
				vkCmdCopyImage(cmd_buf,
				    left_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    sbs_image,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    1, &copy_left);

				// Copy right eye → right half of SBS
				VkImageCopy copy_right;
				copy_right.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
				copy_right.srcSubresource.mipLevel       = 0;
				copy_right.srcSubresource.baseArrayLayer = 0;
				copy_right.srcSubresource.layerCount     = 1;
				copy_right.srcOffset.x = 0; copy_right.srcOffset.y = 0; copy_right.srcOffset.z = 0;
				copy_right.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
				copy_right.dstSubresource.mipLevel       = 0;
				copy_right.dstSubresource.baseArrayLayer = 0;
				copy_right.dstSubresource.layerCount     = 1;
				copy_right.dstOffset.x = (int32_t)eye_w; copy_right.dstOffset.y = 0; copy_right.dstOffset.z = 0;
				copy_right.extent.width  = eye_w;
				copy_right.extent.height = eye_h;
				copy_right.extent.depth  = 1;
				vkCmdCopyImage(cmd_buf,
				    right_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    sbs_image,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    1, &copy_right);

				// Transition all images back
				transition_image_layout(cmd_buf, left_image,
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				transition_image_layout(cmd_buf, right_image,
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				transition_image_layout(cmd_buf, sbs_image,
				    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

				vkEndCommandBuffer(cmd_buf);

				// Submit and wait
				vkResetFences(device, 1, &fence);
				VkSubmitInfo submit_info;
				submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submit_info.pNext                = nullptr;
				submit_info.waitSemaphoreCount   = 0;
				submit_info.pWaitSemaphores      = nullptr;
				submit_info.pWaitDstStageMask    = nullptr;
				submit_info.commandBufferCount   = 1;
				submit_info.pCommandBuffers      = &cmd_buf;
				submit_info.signalSemaphoreCount = 0;
				submit_info.pSignalSemaphores    = nullptr;
				vkQueueSubmit(queue, 1, &submit_info, fence);
				vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
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
			// Release SBS image
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
				displayxr_log("[DisplayXR] Vulkan xrEndFrame: SBS composite OK (eye=%ux%u sbs=%ux%u)\n",
				              eye_w, eye_h, sbs_w, eye_h);
			}
		}

	flush_deferred:
		// Release any remaining deferred swapchains
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

	void *create_shared_texture(uint32_t /*width*/, uint32_t /*height*/) override
	{
		return nullptr; // Hooks layer doesn't manage shared textures
	}

	void destroy_shared_texture() override {}

	void *get_shared_texture_native_ptr() override { return nullptr; }
};

GraphicsBackend *create_vulkan_backend() { return new VulkanBackend(); }

#endif // defined(ENABLE_VULKAN) || defined(__ANDROID__) || ...
