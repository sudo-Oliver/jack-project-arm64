/*!
 * @file MetalGpuResources.mm
 * See MetalGpuResources.h.
 */

#include "MetalGpuResources.h"

#import <Metal/Metal.h>

#include <algorithm>
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

}  // namespace

void metal_install_gpu_resource_backend(void* mtl_device, void* mtl_queue) {
  auto& t = table();
  t.device = (__bridge id<MTLDevice>)mtl_device;
  t.queue = (__bridge id<MTLCommandQueue>)mtl_queue;
  gpu::Backend backend;
  backend.create_texture_rgba8 = metal_create_texture_rgba8;
  backend.destroy_texture = metal_destroy_texture;
  gpu::set_backend(backend);
}

void metal_shutdown_gpu_resource_backend() {
  auto& t = table();
  std::lock_guard<std::mutex> lock(t.mutex);
  t.textures.clear();
  t.free_list.clear();
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
