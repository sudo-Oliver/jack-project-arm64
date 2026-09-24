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

void gl_update_texture_rgba8(u64 handle, u16 w, u16 h, const void* data) {
  GLint old_tex;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, (GLuint)handle);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, data);
  glActiveTexture(old_tex);
}

void gl_destroy_texture(u64 handle) {
  GLuint tex_id = (GLuint)handle;
  glDeleteTextures(1, &tex_id);
}

GLenum gl_target(BufferKind kind) {
  return kind == BufferKind::Index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
}

u64 gl_create_buffer(BufferKind kind, size_t size, const void* data) {
  GLuint buffer;
  glGenBuffers(1, &buffer);
  glBindBuffer(gl_target(kind), buffer);
  glBufferData(gl_target(kind), size, data, GL_STATIC_DRAW);
  return buffer;
}

void gl_update_buffer(BufferKind kind, u64 handle, size_t offset, size_t size, const void* data) {
  glBindBuffer(gl_target(kind), (GLuint)handle);
  glBufferSubData(gl_target(kind), offset, size, data);
}

void gl_destroy_buffer(u64 handle) {
  GLuint buffer = (GLuint)handle;
  glDeleteBuffers(1, &buffer);
}

const Backend kOpenGLBackend = {gl_create_texture_rgba8, gl_update_texture_rgba8,
                                gl_destroy_texture,      gl_create_buffer,
                                gl_update_buffer,        gl_destroy_buffer};

Backend g_backend = kOpenGLBackend;

}  // namespace

void set_backend(const Backend& backend) {
  ASSERT(backend.create_texture_rgba8);
  ASSERT(backend.update_texture_rgba8);
  ASSERT(backend.destroy_texture);
  ASSERT(backend.create_buffer);
  ASSERT(backend.update_buffer);
  ASSERT(backend.destroy_buffer);
  g_backend = backend;
}

u64 create_texture_rgba8(const TextureCreateInfo& info) {
  return g_backend.create_texture_rgba8(info);
}

void update_texture_rgba8(u64 handle, u16 w, u16 h, const void* data) {
  g_backend.update_texture_rgba8(handle, w, h, data);
}

void destroy_texture(u64 handle) {
  g_backend.destroy_texture(handle);
}

u64 create_buffer(BufferKind kind, size_t size, const void* data) {
  return g_backend.create_buffer(kind, size, data);
}

void update_buffer(BufferKind kind, u64 handle, size_t offset, size_t size, const void* data) {
  g_backend.update_buffer(kind, handle, offset, size, data);
}

void destroy_buffer(u64 handle) {
  g_backend.destroy_buffer(handle);
}

}  // namespace gpu
