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

// Every shader that gets compiled into the library, and the entry points it provides.
struct ShaderDef {
  const char* file;        // .metal file name, without the folder
  const char* vert_entry;  // vertex function name, or nullptr
  const char* frag_entry;  // fragment function name, or nullptr
};

// Keep in sync with the files in shaders/. Ported one at a time from the OpenGL renderer.
inline const std::vector<ShaderDef>& all_shaders() {
  static const std::vector<ShaderDef> shaders = {
      {"solid_color.metal", "solid_color_vert", "solid_color_frag"},
  };
  return shaders;
}

}  // namespace metal_shaders
