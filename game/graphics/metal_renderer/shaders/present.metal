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

//
// The upscale. The game is usually drawn at less than the window's resolution, so this is a real
// magnification and the filter matters. A bilinear stretch is what the OpenGL backend's blit
// does and it is visibly soft; this is the Catmull-Rom form of a bicubic, which keeps edges
// crisp without the ringing a sharper kernel would add.
//
// It is nine bilinear taps rather than sixteen point taps: each pair of cubic weights along an
// axis is folded into one offset sample, which the hardware filter then interpolates. Same
// result, a third of the fetches.
//
static float4 sample_catmull_rom(texture2d<float> tex, sampler samp, float2 uv, float2 size) {
  float2 sample_pos = uv * size;
  float2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
  float2 f = sample_pos - tex_pos1;

  // The Catmull-Rom weights for the four taps along each axis.
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);

  // Fold taps 1 and 2 into one bilinear fetch between them.
  float2 w12 = w1 + w2;
  float2 offset12 = w2 / w12;

  float2 tex_pos0 = tex_pos1 - 1.0;
  float2 tex_pos3 = tex_pos1 + 2.0;
  float2 tex_pos12 = tex_pos1 + offset12;

  tex_pos0 /= size;
  tex_pos3 /= size;
  tex_pos12 /= size;

  float4 result = float4(0.0);
  result += tex.sample(samp, float2(tex_pos0.x, tex_pos0.y)) * w0.x * w0.y;
  result += tex.sample(samp, float2(tex_pos12.x, tex_pos0.y)) * w12.x * w0.y;
  result += tex.sample(samp, float2(tex_pos3.x, tex_pos0.y)) * w3.x * w0.y;

  result += tex.sample(samp, float2(tex_pos0.x, tex_pos12.y)) * w0.x * w12.y;
  result += tex.sample(samp, float2(tex_pos12.x, tex_pos12.y)) * w12.x * w12.y;
  result += tex.sample(samp, float2(tex_pos3.x, tex_pos12.y)) * w3.x * w12.y;

  result += tex.sample(samp, float2(tex_pos0.x, tex_pos3.y)) * w0.x * w3.y;
  result += tex.sample(samp, float2(tex_pos12.x, tex_pos3.y)) * w12.x * w3.y;
  result += tex.sample(samp, float2(tex_pos3.x, tex_pos3.y)) * w3.x * w3.y;

  return result;
}

fragment float4 present_frag(PresentVertexOut in [[stage_in]],
                             constant PresentUniforms& u [[buffer(MetalBufferIndexUniforms)]],
                             texture2d<float> scene [[texture(0)]],
                             sampler samp [[sampler(0)]]) {
  if (u.upscale != 0) {
    return clamp(sample_catmull_rom(scene, samp, in.tex_coord, u.scene_size), 0.0, 1.0);
  }
  // Drawn at the window's own resolution: nothing to filter.
  return scene.sample(samp, in.tex_coord);
}
