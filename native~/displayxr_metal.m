// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// IOSurface creation/destruction for zero-copy GPU texture sharing on macOS.
// Unity's Metal context wraps the IOSurface via CreateExternalTexture on the C# side.

#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

#include "displayxr_metal.h"
#include "displayxr_shared_state.h"

// BGRA FourCC as uint32_t: 'B'<<24 | 'G'<<16 | 'R'<<8 | 'A'
#define PIXEL_FORMAT_BGRA 0x42475241

static IOSurfaceRef s_shared_surface = NULL;
static id<MTLTexture> s_shared_texture = nil;

static void
cf_dict_set_int(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
	CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
	CFDictionarySetValue(dict, key, num);
	CFRelease(num);
}

int
displayxr_metal_create_shared_surface(uint32_t width, uint32_t height)
{
	// Release any existing surface
	displayxr_metal_destroy_shared_surface();

	uint32_t bytes_per_element = 4; // BGRA8
	uint32_t bytes_per_row = width * bytes_per_element;
	uint32_t alloc_size = bytes_per_row * height;

	CFMutableDictionaryRef props = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);

	cf_dict_set_int(props, kIOSurfaceWidth, (int32_t)width);
	cf_dict_set_int(props, kIOSurfaceHeight, (int32_t)height);
	cf_dict_set_int(props, kIOSurfaceBytesPerElement, (int32_t)bytes_per_element);
	cf_dict_set_int(props, kIOSurfaceBytesPerRow, (int32_t)bytes_per_row);
	cf_dict_set_int(props, kIOSurfaceAllocSize, (int32_t)alloc_size);
	cf_dict_set_int(props, kIOSurfacePixelFormat, (int32_t)PIXEL_FORMAT_BGRA);

	s_shared_surface = IOSurfaceCreate(props);
	CFRelease(props);

	if (s_shared_surface == NULL) {
		return 0;
	}

	// Create a Metal texture backed by the IOSurface.
	// Unity's CreateExternalTexture needs a MTLTexture*, not an IOSurfaceRef.
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device != nil) {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:width height:height mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		s_shared_texture = [device newTextureWithDescriptor:desc
		    iosurface:s_shared_surface plane:0];
		if (s_shared_texture == nil) {
			fprintf(stderr, "[DisplayXR] Failed to create MTLTexture from IOSurface\n");
		}
	} else {
		fprintf(stderr, "[DisplayXR] Failed to get default Metal device\n");
	}

	// Store in shared state
	DisplayXRState *state = displayxr_get_state();
	state->shared_iosurface = (void *)s_shared_surface;
	state->shared_texture_width = width;
	state->shared_texture_height = height;
	state->shared_texture_ready = 1;

	return 1;
}

void
displayxr_metal_destroy_shared_surface(void)
{
	DisplayXRState *state = displayxr_get_state();

	s_shared_texture = nil; // ARC releases the Metal texture

	if (s_shared_surface != NULL) {
		CFRelease(s_shared_surface);
		s_shared_surface = NULL;
	}

	state->shared_iosurface = NULL;
	state->shared_texture_width = 0;
	state->shared_texture_height = 0;
	state->shared_texture_ready = 0;
}

void *
displayxr_metal_get_iosurface(void)
{
	return (void *)s_shared_surface;
}

void *
displayxr_metal_get_texture(void)
{
	return (__bridge void *)s_shared_texture;
}

// ---------------------------------------------------------------------------
// Find Unity's main window and return its contentView directly.
// The runtime uses this NSView for compositor output.
// ---------------------------------------------------------------------------

static NSView *s_main_view = nil;

void *
displayxr_get_app_main_view(void)
{
	@try {
		// Return cached view if still valid
		if (s_main_view != nil && [s_main_view window] != nil)
			return (__bridge void *)s_main_view;

		NSWindow *window = [[NSApplication sharedApplication] mainWindow];
		if (window == nil)
			window = [[NSApplication sharedApplication] keyWindow];
		if (window == nil) {
			// Fallback: grab the first visible window from the app's window list
			for (NSWindow *w in [[NSApplication sharedApplication] windows]) {
				if ([w isVisible] && [w contentView] != nil) {
					window = w;
					break;
				}
			}
		}
		if (window == nil)
			return NULL;

		s_main_view = [window contentView];

		fprintf(stderr, "[DisplayXR] Found main window '%s' contentView (%dx%d)\n",
		        [[window title] UTF8String],
		        (int)[s_main_view bounds].size.width,
		        (int)[s_main_view bounds].size.height);

		return (__bridge void *)s_main_view;
	} @catch (NSException *e) {
		fprintf(stderr, "[DisplayXR] Exception getting main view: %s\n",
		        [[e reason] UTF8String]);
		return NULL;
	}
}

