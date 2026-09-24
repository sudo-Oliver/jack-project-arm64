/*!
 * @file MetalGpuResources.mm
 * See MetalGpuResources.h.
 */

#include "MetalGpuResources.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/gpu_resources.h"

namespace {

struct TextureTable {
  std::mutex mutex;
  std::vector<id<MTLTexture>> textures;  // indexed by handle
  std::vector<u32> free_list;
  std::vector<id<MTLBuffer>> buffers;  // indexed by handle, separate space from textures
  std::vector<u32> buffer_free_list;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
};

TextureTable& table() {
  static TextureTable t;
  return t;
}

// The PS2 textures arrive as GL_UNSIGNED_INT_8_8_8_8_REV over GL_RGBA, which is the same byte
// order as Metal's BGRA8Unorm.
constexpr MTLPixelFormat kTextureFormat = MTLPixelFormatBGRA8Unorm;

u32 mip_level_count(u16 w, u16 h) {
  u32 levels = 1;
  u32 size = std::max<u32>(w, h);
  while (size > 1) {
    size >>= 1;
    levels++;
  }
  return levels;
}

u64 metal_create_texture_rgba8(const gpu::TextureCreateInfo& info) {
  auto& t = table();
  ASSERT_MSG(t.device, "metal_create_texture_rgba8 before the Metal backend was installed");
  if (info.w == 0 || info.h == 0) {
    return gpu::kInvalidHandle;
  }

  const u32 levels = info.mipmap ? mip_level_count(info.w, info.h) : 1;
  MTLTextureDescriptor* desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kTextureFormat
                                                         width:info.w
                                                        height:info.h
                                                     mipmapped:(levels > 1)];
  desc.mipmapLevelCount = levels;
  desc.usage = MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;

  id<MTLTexture> tex = [t.device newTextureWithDescriptor:desc];
  if (!tex) {
    lg::error("[Metal] failed to create a {}x{} texture", info.w, info.h);
    return gpu::kInvalidHandle;
  }

  if (info.data) {
    [tex replaceRegion:MTLRegionMake2D(0, 0, info.w, info.h)
           mipmapLevel:0
             withBytes:info.data
           bytesPerRow:(NSUInteger)info.w * 4];
  }

  if (levels > 1 && info.data) {
    // Metal has no glGenerateMipmap; the equivalent is a blit encoder, which needs a command
    // buffer. Wait for it so the texture is complete by the time the handle is handed out -- this
    // runs on the loader thread, not in a frame.
    id<MTLCommandBuffer> cmd = [t.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit generateMipmapsForTexture:tex];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
  }

  std::lock_guard<std::mutex> lock(t.mutex);
  u32 handle;
  if (t.free_list.empty()) {
    handle = (u32)t.textures.size();
    t.textures.push_back(tex);
  } else {
    handle = t.free_list.back();
    t.free_list.pop_back();
    t.textures[handle] = tex;
  }
  return handle;
}

void metal_destroy_texture(u64 handle) {
  if (handle == gpu::kInvalidHandle) {
    return;
  }
  auto& t = table();
  std::lock_guard<std::mutex> lock(t.mutex);
  if (handle >= t.textures.size() || !t.textures[handle]) {
    return;
  }
  t.textures[handle] = nil;  // ARC releases it
  t.free_list.push_back((u32)handle);
}

u64 metal_create_buffer(gpu::BufferKind /*kind*/, size_t size, const void* data) {
  auto& t = table();
  ASSERT_MSG(t.device, "metal_create_buffer before the Metal backend was installed");
  if (size == 0) {
    return gpu::kInvalidHandle;
  }
  // Shared storage: the loader writes these in chunks from the game thread and the GPU reads them
  // without a copy, which is what unified memory is for.
  id<MTLBuffer> buffer = data ? [t.device newBufferWithBytes:data
                                                      length:size
                                                     options:MTLResourceStorageModeShared]
                              : [t.device newBufferWithLength:size
                                                      options:MTLResourceStorageModeShared];
  if (!buffer) {
    lg::error("[Metal] failed to create a {}-byte buffer", size);
    return gpu::kInvalidHandle;
  }

  std::lock_guard<std::mutex> lock(t.mutex);
  u32 handle;
  if (t.buffer_free_list.empty()) {
    handle = (u32)t.buffers.size();
    t.buffers.push_back(buffer);
  } else {
    handle = t.buffer_free_list.back();
    t.buffer_free_list.pop_back();
    t.buffers[handle] = buffer;
  }
  return handle;
}

void metal_update_buffer(gpu::BufferKind /*kind*/,
                         u64 handle,
                         size_t offset,
                         size_t size,
                         const void* data) {
  auto& t = table();
  id<MTLBuffer> buffer = nil;
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    if (handle == gpu::kInvalidHandle || handle >= t.buffers.size()) {
      return;
    }
    buffer = t.buffers[handle];
  }
  if (!buffer || !data) {
    return;
  }
  ASSERT_MSG(offset + size <= [buffer length], "metal_update_buffer writes past the buffer");
  memcpy((u8*)[buffer contents] + offset, data, size);
}

void metal_destroy_buffer(u64 handle) {
  if (handle == gpu::kInvalidHandle) {
    return;
  }
  auto& t = table();
  std::lock_guard<std::mutex> lock(t.mutex);
  if (handle >= t.buffers.size() || !t.buffers[handle]) {
    return;
  }
  t.buffers[handle] = nil;  // ARC releases it
  t.buffer_free_list.push_back((u32)handle);
}

}  // namespace

void metal_install_gpu_resource_backend(void* mtl_device, void* mtl_queue) {
  auto& t = table();
  t.device = (__bridge id<MTLDevice>)mtl_device;
  t.queue = (__bridge id<MTLCommandQueue>)mtl_queue;
  gpu::Backend backend;
  backend.create_texture_rgba8 = metal_create_texture_rgba8;
  backend.destroy_texture = metal_destroy_texture;
  backend.create_buffer = metal_create_buffer;
  backend.update_buffer = metal_update_buffer;
  backend.destroy_buffer = metal_destroy_buffer;
  gpu::set_backend(backend);
}

void metal_shutdown_gpu_resource_backend() {
  auto& t = table();
  std::lock_guard<std::mutex> lock(t.mutex);
  t.textures.clear();
  t.free_list.clear();
  t.buffers.clear();
  t.buffer_free_list.clear();
  t.device = nil;
  t.queue = nil;
}

id<MTLTexture> metal_texture_from_handle(u64 handle) {
  auto& t = table();
  std::lock_guard<std::mutex> lock(t.mutex);
  if (handle == gpu::kInvalidHandle || handle >= t.textures.size()) {
    return nil;
  }
  return t.textures[handle];
}

id<MTLBuffer> metal_buffer_from_handle(u64 handle) {
  auto& t = table();
  std::lock_guard<std::mutex> lock(t.mutex);
  if (handle == gpu::kInvalidHandle || handle >= t.buffers.size()) {
    return nil;
  }
  return t.buffers[handle];
}
