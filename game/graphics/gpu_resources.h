#pragma once

/*!
 * @file gpu_resources.h
 * Backend-neutral GPU resource creation.
 *
 * The texture pipeline (PS2 format conversion, TexturePool bookkeeping, the loader stages) is
 * identical for OpenGL and Metal -- only the handful of calls that actually allocate a GPU object
 * differ. Those calls go through this interface so that code can be shared instead of duplicated
 * per backend.
 *
 * A handle is an opaque u64. Under OpenGL it is the GLuint itself, so existing renderer code that
 * binds a handle directly keeps working unchanged. Under Metal it is an index into a table of
 * Objective-C objects owned by the Metal backend.
 *
 * Every handle must fit in 32 bits: TexturePool and the renderers store texture handles as GLuint,
 * so a Metal handle that did not fit would be silently truncated on the way through.
 *
 * The backend is installed once during graphics init (OpenGL is the default) and every function
 * here is safe to call from the loader thread.
 */

#include "common/common_types.h"

namespace gpu {

constexpr u64 kInvalidHandle = (u64)-1;

struct TextureCreateInfo {
  u16 w = 0;
  u16 h = 0;
  const void* data = nullptr;  // RGBA8, w * h * 4 bytes
  bool mipmap = true;
  bool anisotropic = true;
  // Sets clamp-to-edge + linear filtering on the texture object itself. The level textures leave
  // this off: their sampler state is set by the renderer at bind time.
  bool clamp_and_linear = false;
};

// Function table for one backend. Every entry must be set.
struct Backend {
  u64 (*create_texture_rgba8)(const TextureCreateInfo& info) = nullptr;
  void (*destroy_texture)(u64 handle) = nullptr;
};

// Install the backend. Call once, before any resource is created.
void set_backend(const Backend& backend);

u64 create_texture_rgba8(const TextureCreateInfo& info);
void destroy_texture(u64 handle);

}  // namespace gpu
