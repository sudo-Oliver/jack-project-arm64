//
// The final pass: copy the scene texture onto the drawable.
//
// The game is drawn into an offscreen colour texture rather than straight into the drawable, so
// that a renderer that has to read the frame so far (the depth cue, the sprite distorter) can
// sample it. This is what puts the result on screen afterwards. It has no OpenGL counterpart --
// there the equivalent is the glBlitFramebuffer out of the render FBO.
//
// The vertex shader takes no buffer: three vertices covering the screen are cheaper to compute
// from the vertex id than to bind.
//

#include <metal_stdlib>
using namespace metal;

struct PresentVertexOut {
  float4 position [[position]];
  float2 tex_coord;
};

vertex PresentVertexOut present_vert(uint vid [[vertex_id]]) {
  // One triangle covering the clip-space square twice over: (-1,-1), (3,-1), (-1,3).
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  PresentVertexOut out;
  float2 p = positions[vid];
  out.position = float4(p, 0.0, 1.0);
  // Texture space has y down, clip space has y up.
  out.tex_coord = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
  return out;
}

fragment float4 present_frag(PresentVertexOut in [[stage_in]],
                             texture2d<float> scene [[texture(0)]],
                             sampler samp [[sampler(0)]]) {
  return scene.sample(samp, in.tex_coord);
}
