// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// IOSurface creation/destruction for the standalone preview session.
// Independent from the hook chain's IOSurface (displayxr_metal.m).

#import <Metal/Metal.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

#include "displayxr_standalone_metal.h"

#define SA_PIXEL_FORMAT_BGRA 0x42475241

static IOSurfaceRef s_sa_surface = NULL;
static id<MTLTexture> s_sa_texture = nil;
static id<MTLDevice> s_sa_device = nil;
static id<MTLCommandQueue> s_sa_queue = nil;

static void
sa_cf_dict_set_int(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
	CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
	CFDictionarySetValue(dict, key, num);
	CFRelease(num);
}

int
displayxr_sa_metal_create(uint32_t width, uint32_t height)
{
	displayxr_sa_metal_destroy();

	uint32_t bpe = 4;
	uint32_t bpr = width * bpe;
	uint32_t alloc = bpr * height;

	CFMutableDictionaryRef props = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);

	sa_cf_dict_set_int(props, kIOSurfaceWidth, (int32_t)width);
	sa_cf_dict_set_int(props, kIOSurfaceHeight, (int32_t)height);
	sa_cf_dict_set_int(props, kIOSurfaceBytesPerElement, (int32_t)bpe);
	sa_cf_dict_set_int(props, kIOSurfaceBytesPerRow, (int32_t)bpr);
	sa_cf_dict_set_int(props, kIOSurfaceAllocSize, (int32_t)alloc);
	sa_cf_dict_set_int(props, kIOSurfacePixelFormat, (int32_t)SA_PIXEL_FORMAT_BGRA);

	s_sa_surface = IOSurfaceCreate(props);
	CFRelease(props);

	if (!s_sa_surface) {
		fprintf(stderr, "[DisplayXR-SA] Failed to create IOSurface\n");
		return 0;
	}

	if (!s_sa_device) {
		s_sa_device = MTLCreateSystemDefaultDevice();
	}
	if (s_sa_device) {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			width:width height:height mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		s_sa_texture = [s_sa_device newTextureWithDescriptor:desc
			iosurface:s_sa_surface plane:0];
		if (!s_sa_texture) {
			fprintf(stderr, "[DisplayXR-SA] Failed to create MTLTexture from IOSurface\n");
		}
	}

	fprintf(stderr, "[DisplayXR-SA] IOSurface created: %ux%u\n", width, height);
	return 1;
}

void
displayxr_sa_metal_destroy(void)
{
	s_sa_texture = nil;
	s_sa_queue = nil;
	s_sa_device = nil;
	if (s_sa_surface) {
		CFRelease(s_sa_surface);
		s_sa_surface = NULL;
	}
}

void *
displayxr_sa_metal_get_iosurface(void)
{
	return (void *)s_sa_surface;
}

void *
displayxr_sa_metal_get_texture(void)
{
	return (__bridge void *)s_sa_texture;
}

void *
displayxr_sa_metal_get_command_queue(void)
{
	if (!s_sa_device) {
		s_sa_device = MTLCreateSystemDefaultDevice();
	}
	if (!s_sa_queue && s_sa_device) {
		s_sa_queue = [s_sa_device newCommandQueue];
	}
	return (__bridge void *)s_sa_queue;
}

int
displayxr_sa_metal_blit(void *src_ptr, void *dst_ptr)
{
	if (!src_ptr || !dst_ptr || !s_sa_queue) return 0;

	id<MTLTexture> src = (__bridge id<MTLTexture>)src_ptr;
	id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_ptr;

	id<MTLCommandBuffer> cmd = [s_sa_queue commandBuffer];
	if (!cmd) return 0;

	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
	if (!blit) return 0;

	// Copy the overlapping region (handles size mismatch)
	NSUInteger w = MIN(src.width, dst.width);
	NSUInteger h = MIN(src.height, dst.height);

	[blit copyFromTexture:src
	          sourceSlice:0
	          sourceLevel:0
	         sourceOrigin:MTLOriginMake(0, 0, 0)
	           sourceSize:MTLSizeMake(w, h, 1)
	            toTexture:dst
	     destinationSlice:0
	     destinationLevel:0
	    destinationOrigin:MTLOriginMake(0, 0, 0)];

	[blit endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];

	return 1;
}
