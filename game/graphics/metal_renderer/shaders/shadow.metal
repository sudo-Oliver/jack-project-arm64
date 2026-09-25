//
// Metal port of game/graphics/opengl_renderer/shaders/shadow.{vert,frag}
//
// The shadow volumes. The vertices arrive already in the GS's screen-space units, so this is only
// the fixed rescale into clip space; the shading is a flat colour the renderer sets per pass.
//
// Vertex layout is ShadowRendererCore::Vertex.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct ShadowVertexIn {
  float3 position_in [[attribute(0)]];
};

struct ShadowVertexOut {
  float4 position [[position]];
};

vertex ShadowVertexOut shadow_vert(ShadowVertexIn in [[stage_in]],
                                   constant ShadowUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  ShadowVertexOut out;
  // Note: position.y is multiplied by 32 instead of 16 to undo the half-height for interlacing.
  float4 pos = float4((in.position_in.x - 0.5) * 16.0, -(in.position_in.y - 0.5) * 32.0,
                      in.position_in.z * 2.0 - 1.0, 1.0);
  // scissoring area adjust
  pos.y *= u.scissor_adjust;
  // OpenGL clips z to [-w, w], Metal to [0, w]. With w = 1 that undoes the *2-1 above exactly.
  pos.z = (pos.z + pos.w) * 0.5;
  out.position = pos;
  return out;
}

fragment float4 shadow_frag(ShadowVertexOut in [[stage_in]],
                            constant ShadowUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  return u.color * 2.0;
}
