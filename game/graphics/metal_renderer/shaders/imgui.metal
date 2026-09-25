//
// The debug GUI's draw lists.
//
// ImGui hands over one vertex buffer and one index buffer per frame plus a list of clipped
// commands. This is the whole of what a backend has to draw them: a position scaled from screen
// pixels into clip space, a vertex colour, and one texture.
//
// Vertex layout is ImDrawVert: float2 pos, float2 uv, uchar4 col.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct ImGuiVertexIn {
  float2 position [[attribute(0)]];
  float2 tex_coord [[attribute(1)]];
  uchar4 color [[attribute(2)]];
};

struct ImGuiVertexOut {
  float4 position [[position]];
  float4 color;
  float2 tex_coord;
};

vertex ImGuiVertexOut imgui_vert(ImGuiVertexIn in [[stage_in]],
                                 constant ImGuiUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  ImGuiVertexOut out;
  // From ImGui's pixel coordinates (y down from the top left) to clip space.
  out.position = float4(in.position * u.scale + u.translate, 0.0, 1.0);
  out.color = float4(in.color) / 255.0;
  out.tex_coord = in.tex_coord;
  return out;
}

fragment float4 imgui_frag(ImGuiVertexOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]]) {
  return in.color * tex.sample(samp, in.tex_coord);
}
