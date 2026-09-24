#pragma once

/*!
 * @file MetalGpuResources.h
 * Metal implementation of the backend-neutral GPU resource interface in game/graphics/
 * gpu_resources.h.
 *
 * Handles are indices into a table of id<MTLTexture>, so they fit in the 32 bits that TexturePool
 * and the renderers store them in. The table is guarded by a mutex because the loader thread
 * creates textures while the render thread reads them.
 *
 * The plain-C++ half of this header is what the runtime calls; the Objective-C half is for the
 * Metal renderers, which need the object back out of a handle.
 */

#include "common/common_types.h"

// Installs the Metal backend into gpu::set_backend. Call once, after the MTLDevice exists.
// Takes the device and a command queue for the blit work that mipmap generation needs.
void metal_install_gpu_resource_backend(void* mtl_device, void* mtl_queue);

// Releases every texture still in the table.
void metal_shutdown_gpu_resource_backend();

#ifdef __OBJC__
#import <Metal/Metal.h>

// Both return nil for a handle that was never created or has been destroyed. Texture and buffer
// handles live in separate spaces, so the same number can mean both.
id<MTLTexture> metal_texture_from_handle(u64 handle);
id<MTLBuffer> metal_buffer_from_handle(u64 handle);
#endif
