//
// Metal port of game/graphics/opengl_renderer/shaders/solid_color.{vert,frag}
//
// GLSL had a `vec2 position_in` vertex attribute and a `vec4 fragment_color` uniform. Metal has
// no free-floating uniforms, so the colour arrives in a buffer instead; see SolidColorUniforms
// in metal_shader_types.h, which must stay in sync with this file.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct VertexOut {
  float4 position [[position]];
};

vertex VertexOut solid_color_vert(uint vid [[vertex_id]],
                                  const device float2* positions [[buffer(MetalBufferIndexVertex)]]) {
  VertexOut out;
  out.position = float4(positions[vid], 0.0, 1.0);
  return out;
}

fragment float4 solid_color_frag(VertexOut in [[stage_in]],
                                 constant SolidColorUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  return u.fragment_color;
}
