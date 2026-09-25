//
// Metal port of game/graphics/opengl_renderer/shaders/sprite_distort_instanced.{vert,frag}
//
// The sprite distorter: the heat haze over a vent, the shimmer under water. Each sprite is a fan
// of "slices" that samples the frame drawn so far at an offset position, which is what bends the
// image behind it.
//
// Only the instanced form is ported. The OpenGL backend keeps a non-instanced path as a fallback
// for drivers without instancing; Metal has had it since the first release, so there is nothing
// for that path to fall back to.
//
// The per-vertex mesh is the sine table with the sprite-specific parts removed, one mesh per
// sprite "resolution" (3 to 11 slices); the per-instance data is the sprite's position, scale and
// texture coordinate.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct DistortVertexIn {
  float3 xyz [[attribute(0)]];  // position from the sine table
  float2 st [[attribute(1)]];   // tex coord from the sine table
  float4 instance_xyz_s [[attribute(2)]];    // sprite position + texture S
  float4 instance_scale_t [[attribute(3)]];  // sprite scale + texture T
};

struct DistortVertexOut {
  float4 position [[position]];
  float4 fragment_color [[flat]];
  float2 tex_coord;
};

vertex DistortVertexOut sprite_distort_vert(DistortVertexIn in [[stage_in]],
                                            uint vid [[vertex_id]],
                                            constant SpriteDistortUniforms& u
                                            [[buffer(MetalBufferIndexUniforms)]]) {
  DistortVertexOut out;
  out.fragment_color = u.u_color;

  // The VU program operated on each slice separately, which here is every 5 vertices: two scaled
  // by sizeX, two by sizeY (sizeZ for the texture coordinate), and the centre vertex untouched.
  float slice_vert_id = float(vid % 5);

  float2 texture_coord = float2(in.instance_xyz_s.w, in.instance_scale_t.w);
  if (slice_vert_id < 2.0) {
    texture_coord += in.st * in.instance_scale_t.x;
  } else if (slice_vert_id < 4.0) {
    texture_coord += in.st * in.instance_scale_t.z;
  }
  out.tex_coord = texture_coord;

  float3 position = in.instance_xyz_s.xyz;
  if (slice_vert_id < 2.0) {
    position += in.xyz * in.instance_scale_t.x;
  } else if (slice_vert_id < 4.0) {
    position += in.xyz * in.instance_scale_t.y;
  }

  float4 transformed = float4(position, 1.0);

  // correct xy offset
  transformed.xy -= 2048.0;
  // correct z scale
  transformed.z /= 8388608.0;
  transformed.z -= 1.0;
  // correct xy scale
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.y *= u.height_scale;
  // OpenGL clips z to [-w, w], Metal to [0, w]; w is 1 here.
  transformed.z = (transformed.z + 1.0) * 0.5;

  out.position = transformed;
  return out;
}

fragment float4 sprite_distort_frag(DistortVertexOut in [[stage_in]],
                                    constant SpriteDistortUniforms& u
                                    [[buffer(MetalBufferIndexUniforms)]],
                                    texture2d<float> framebuffer_tex [[texture(0)]],
                                    sampler samp [[sampler(0)]]) {
  float4 color = in.fragment_color;

  // correct color
  color *= 2.0;

  // correct texture coordinates
  float2 texture_coords =
      float2(in.tex_coord.x, (1.0 - in.tex_coord.y) - (1.0 - (u.scissor_height / 512.0)) / 2.0);

  return color * framebuffer_tex.sample(samp, texture_coords);
}
