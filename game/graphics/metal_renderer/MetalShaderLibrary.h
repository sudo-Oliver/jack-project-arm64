#pragma once

/*!
 * @file MetalShaderLibrary.h
 * Loads the MSL shaders from game/graphics/metal_renderer/shaders/ and compiles them at runtime.
 *
 * Runtime compilation rather than an offline .metallib, for two reasons: it needs no Metal
 * Toolchain install (`xcodebuild -downloadComponent MetalToolchain`), and it matches how the
 * OpenGL backend already handles GLSL, so editing a shader does not mean rebuilding the game.
 *
 * MTLCompileOptions has no include path, so `#include "..."` directives are expanded here before
 * handing the source to Metal. That keeps metal_shader_types.h as the single definition of the
 * layouts shared with C++.
 */

#include <string>
#include <vector>

#include "common/common_types.h"

namespace metal_shaders {

// Folder holding the .metal sources, relative to the project root.
inline constexpr char kShaderFolder[] = "game/graphics/metal_renderer/shaders/";

// Read `name` from the shader folder and expand its `#include "..."` directives.
// Throws std::runtime_error if a file is missing.
std::string load_source_with_includes(const std::string& name);

// Every shader in all_shaders(), concatenated into the one source the library is compiled from.
// Shared headers are expanded once for the whole program, not once per file: `#pragma once` is
// useless here because the expansion is textual, and a second copy of metal_shader_types.h is a
// redefinition error rather than a no-op.
std::string load_all_sources();

// Every shader that gets compiled into the library, and the entry points it provides.
struct ShaderDef {
  const char* file;        // .metal file name, without the folder
  const char* vert_entry;  // vertex function name, or nullptr
  const char* frag_entry;  // fragment function name, or nullptr
};

// Keep in sync with the files in shaders/. Ported one at a time from the OpenGL renderer.
inline const std::vector<ShaderDef>& all_shaders() {
  static const std::vector<ShaderDef> shaders = {
      {"tfrag3.metal", "tfrag3_vert", "tfrag3_frag"},
      {"shrub.metal", "shrub_vert", "shrub_frag"},
      {"tie_wind.metal", "tie_wind_vert", "tie_wind_frag"},
      {"merc2.metal", "merc2_vert", "merc2_frag"},
      {"emerc.metal", "emerc_vert", "emerc_frag"},
      {"sky_blend.metal", "sky_blend_vert", "sky_blend_frag"},
      {"eye.metal", "eye_vert", "eye_frag"},
      {"ocean_common.metal", "ocean_common_vert", "ocean_common_frag"},
      {"ocean_texture.metal", "ocean_texture_vert", "ocean_texture_frag"},
      {"ocean_texture_mipmap.metal", "ocean_texture_mipmap_vert", "ocean_texture_mipmap_frag"},
      {"sprite3.metal", "sprite3_vert", "sprite3_frag"},
      {"shadow.metal", "shadow_vert", "shadow_frag"},
      {"generic.metal", "generic_vert", "generic_frag"},
      {"depth_cue.metal", "depth_cue_vert", "depth_cue_frag"},
      {"present.metal", "present_vert", "present_frag"},
      {"sprite_distort.metal", "sprite_distort_vert", "sprite_distort_frag"},
      {"direct_basic_textured.metal", "direct_basic_textured_vert",
       "direct_basic_textured_frag"},
  };
  return shaders;
}

}  // namespace metal_shaders
