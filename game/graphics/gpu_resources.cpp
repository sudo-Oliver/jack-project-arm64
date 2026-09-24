/*!
 * @file gpu_resources.cpp
 * Dispatch for backend-neutral GPU resource creation, plus the OpenGL implementation.
 *
 * OpenGL is the default backend, so code that runs before any renderer is selected behaves
 * exactly as it did before this indirection existed. The Metal backend installs itself in
 * metal_make_display.
 */

#include "gpu_resources.h"

#include "common/util/Assert.h"

#include "game/graphics/pipelines/opengl.h"

namespace gpu {

namespace {

u64 gl_create_texture_rgba8(const TextureCreateInfo& info) {
  GLuint tex_id;
  glGenTextures(1, &tex_id);
  GLint old_tex;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, info.w, info.h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV,
               info.data);
  if (info.mipmap) {
    glGenerateMipmap(GL_TEXTURE_2D);
  }
  if (info.anisotropic) {
    float aniso = 0.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
  }
  if (info.clamp_and_linear) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }
  glActiveTexture(old_tex);
  return tex_id;
}

void gl_destroy_texture(u64 handle) {
  GLuint tex_id = (GLuint)handle;
  glDeleteTextures(1, &tex_id);
}

const Backend kOpenGLBackend = {gl_create_texture_rgba8, gl_destroy_texture};

Backend g_backend = kOpenGLBackend;

}  // namespace

void set_backend(const Backend& backend) {
  ASSERT(backend.create_texture_rgba8);
  ASSERT(backend.destroy_texture);
  g_backend = backend;
}

u64 create_texture_rgba8(const TextureCreateInfo& info) {
  return g_backend.create_texture_rgba8(info);
}

void destroy_texture(u64 handle) {
  g_backend.destroy_texture(handle);
}

}  // namespace gpu
