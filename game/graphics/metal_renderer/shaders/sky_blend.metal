//
// Metal port of game/graphics/opengl_renderer/shaders/sky_blend.{vert,frag}
//
// The sky and cloud textures the sky geometry samples are built by blending several source
// textures, one per level and time of day, into a 32x32 and a 64x64 target. Each blend is one
// full-target quad with an intensity, added onto what is already there.
//
// The vertex positions are generated from the vertex id, so there is no vertex buffer: the target
// is always the whole quad.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct SkyBlendVertexOut {
  float4 position [[position]];
  float2 tex_coord;
  float intensity;
};

vertex SkyBlendVertexOut sky_blend_vert(uint vid [[vertex_id]],
                                        constant SkyBlendUniforms& u
                                            [[buffer(MetalBufferIndexUniforms)]]) {
  // Two triangles covering the target, in the order the GLSL's vertex buffer had them.
  const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1),
                             float2(1, 0), float2(0, 1), float2(1, 1)};
  float2 c = corners[vid];
  SkyBlendVertexOut out;
  out.position = float4(c.x * 2.0 - 1.0, c.y * 2.0 - 1.0, 0.0, 1.0);
  out.tex_coord = c;
  out.intensity = u.intensity;
  return out;
}

fragment float4 sky_blend_frag(SkyBlendVertexOut in [[stage_in]],
                               texture2d<float> tex [[texture(0)]],
                               sampler samp [[sampler(0)]]) {
  return tex.sample(samp, in.tex_coord) * in.intensity;
}
