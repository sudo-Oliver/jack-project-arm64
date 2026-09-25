/*!
 * @file MetalImGui.mm
 * See MetalImGui.h.
 */

#include "MetalImGui.h"

#import <Metal/Metal.h>

#include <array>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalRenderState.h"

#include "metal_shader_types.h"
#include "third-party/imgui/imgui.h"

namespace {
// The buffers grow to whatever a frame needs and stay there; the debug GUI's size is stable.
constexpr NSUInteger kInitialVertexBytes = 64 * 1024;
constexpr NSUInteger kInitialIndexBytes = 16 * 1024;
}  // namespace

struct MetalImGui::Impl {
  id<MTLDevice> device = nil;
  id<MTLRenderPipelineState> pso = nil;
  id<MTLDepthStencilState> depth_state = nil;
  id<MTLSamplerState> sampler = nil;
  id<MTLTexture> font_texture = nil;
  bool ready = false;

  std::array<id<MTLBuffer>, kMetalFramesInFlight> vertex_buffers = {nil, nil, nil};
  std::array<id<MTLBuffer>, kMetalFramesInFlight> index_buffers = {nil, nil, nil};
  int frame = 0;

  // Grows a buffer in place when a frame needs more than it holds.
  id<MTLBuffer> ensure(__strong id<MTLBuffer>& buffer, NSUInteger needed) {
    if (!buffer || [buffer length] < needed) {
      NSUInteger size = buffer ? [buffer length] : 1024;
      while (size < needed) {
        size *= 2;
      }
      buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    }
    return buffer;
  }
};

MetalImGui::MetalImGui() : m_impl(std::make_unique<Impl>()) {}

MetalImGui::~MetalImGui() = default;

bool MetalImGui::init(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat color_format) {
  m_impl->device = device;

  id<MTLFunction> vert = [library newFunctionWithName:@"imgui_vert"];
  id<MTLFunction> frag = [library newFunctionWithName:@"imgui_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] imgui shader entry points missing from the library");
    return false;
  }

  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat2;
  vd.attributes[0].offset = offsetof(ImDrawVert, pos);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat2;
  vd.attributes[1].offset = offsetof(ImDrawVert, uv);
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatUChar4;
  vd.attributes[2].offset = offsetof(ImDrawVert, col);
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(ImDrawVert);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vert;
  desc.fragmentFunction = frag;
  desc.vertexDescriptor = vd;
  auto* color = desc.colorAttachments[0];
  color.pixelFormat = color_format;
  color.blendingEnabled = YES;
  color.rgbBlendOperation = MTLBlendOperationAdd;
  color.alphaBlendOperation = MTLBlendOperationAdd;
  color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  color.sourceAlphaBlendFactor = MTLBlendFactorOne;
  color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

  NSError* err = nil;
  m_impl->pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!m_impl->pso) {
    lg::error("[Metal] imgui pipeline failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
    return false;
  }

  // The GUI is drawn over the finished frame, so no depth at all.
  MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
  dd.depthCompareFunction = MTLCompareFunctionAlways;
  dd.depthWriteEnabled = NO;
  m_impl->depth_state = [device newDepthStencilStateWithDescriptor:dd];

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = MTLSamplerAddressModeRepeat;
  sd.tAddressMode = MTLSamplerAddressModeRepeat;
  m_impl->sampler = [device newSamplerStateWithDescriptor:sd];

  // The font atlas.
  {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels || width <= 0 || height <= 0) {
      lg::error("[Metal] imgui font atlas is empty");
      return false;
    }
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    m_impl->font_texture = [device newTextureWithDescriptor:td];
    [m_impl->font_texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                            mipmapLevel:0
                              withBytes:pixels
                            bytesPerRow:(NSUInteger)width * 4];
    // ImGui keeps whatever the backend puts here and hands it back per draw command. Nothing
    // else here binds a texture of its own, so the atlas is the only one there will be.
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);
  }

  for (int i = 0; i < kMetalFramesInFlight; i++) {
    m_impl->vertex_buffers[i] = [device newBufferWithLength:kInitialVertexBytes
                                                    options:MTLResourceStorageModeShared];
    m_impl->index_buffers[i] = [device newBufferWithLength:kInitialIndexBytes
                                                   options:MTLResourceStorageModeShared];
  }

  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = "opengoal_metal";
  // The draw lists are packed into one buffer per frame and addressed with baseVertex, so ImGui
  // may keep a list past 64k vertices rather than splitting it.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

  m_impl->ready = true;
  lg::info("[Metal] debug GUI ready");
  return true;
}

void MetalImGui::render(ImDrawData* draw_data,
                        id<MTLRenderCommandEncoder> encoder,
                        u32 target_width,
                        u32 target_height) {
  if (!m_impl->ready || !draw_data || !encoder || draw_data->CmdListsCount == 0) {
    return;
  }
  if (draw_data->DisplaySize.x <= 0 || draw_data->DisplaySize.y <= 0) {
    return;
  }

  const int frame = m_impl->frame;
  m_impl->frame = (m_impl->frame + 1) % kMetalFramesInFlight;

  const NSUInteger vertex_bytes = (NSUInteger)draw_data->TotalVtxCount * sizeof(ImDrawVert);
  const NSUInteger index_bytes = (NSUInteger)draw_data->TotalIdxCount * sizeof(ImDrawIdx);
  if (vertex_bytes == 0 || index_bytes == 0) {
    return;
  }
  id<MTLBuffer> vbuf = m_impl->ensure(m_impl->vertex_buffers[frame], vertex_bytes);
  id<MTLBuffer> ibuf = m_impl->ensure(m_impl->index_buffers[frame], index_bytes);
  if (!vbuf || !ibuf) {
    return;
  }

  // ImGui's draw lists are separate allocations; pack them into one buffer and remember where
  // each one started, which is what baseVertex and the index offset are for.
  auto* vtx_out = (ImDrawVert*)[vbuf contents];
  auto* idx_out = (ImDrawIdx*)[ibuf contents];
  u32 vtx_offset = 0, idx_offset = 0;
  struct ListOffsets {
    u32 vtx;
    u32 idx;
  };
  std::vector<ListOffsets> offsets;
  offsets.reserve(draw_data->CmdListsCount);
  for (int n = 0; n < draw_data->CmdListsCount; n++) {
    const ImDrawList* list = draw_data->CmdLists[n];
    offsets.push_back({vtx_offset, idx_offset});
    memcpy(vtx_out + vtx_offset, list->VtxBuffer.Data,
           list->VtxBuffer.Size * sizeof(ImDrawVert));
    memcpy(idx_out + idx_offset, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
    vtx_offset += list->VtxBuffer.Size;
    idx_offset += list->IdxBuffer.Size;
  }

  // ImGui reports sizes in its own coordinates; the target may be a different number of pixels.
  const float scale_x = draw_data->FramebufferScale.x;
  const float scale_y = draw_data->FramebufferScale.y;

  ImGuiUniforms u{};
  u.scale = {2.0f / draw_data->DisplaySize.x, -2.0f / draw_data->DisplaySize.y};
  u.translate = {-1.0f - draw_data->DisplayPos.x * u.scale.x,
                 1.0f + draw_data->DisplayPos.y * (2.0f / draw_data->DisplaySize.y)};

  [encoder setRenderPipelineState:m_impl->pso];
  [encoder setDepthStencilState:m_impl->depth_state];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setVertexBuffer:vbuf offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  [encoder setFragmentTexture:m_impl->font_texture atIndex:0];
  [encoder setFragmentSamplerState:m_impl->sampler atIndex:0];
  [encoder setViewport:(MTLViewport){0.0, 0.0, (double)target_width, (double)target_height, 0.0,
                                     1.0}];

  const ImVec2 clip_off = draw_data->DisplayPos;
  for (int n = 0; n < draw_data->CmdListsCount; n++) {
    const ImDrawList* list = draw_data->CmdLists[n];
    for (int cmd_i = 0; cmd_i < list->CmdBuffer.Size; cmd_i++) {
      const ImDrawCmd& cmd = list->CmdBuffer[cmd_i];
      if (cmd.UserCallback) {
        cmd.UserCallback(list, &cmd);
        continue;
      }
      if (cmd.ElemCount == 0) {
        continue;
      }

      // The clip rectangle, in target pixels and clamped: Metal rejects a scissor rect that
      // reaches outside the attachment, where OpenGL silently clamps.
      double x0 = (cmd.ClipRect.x - clip_off.x) * scale_x;
      double y0 = (cmd.ClipRect.y - clip_off.y) * scale_y;
      double x1 = (cmd.ClipRect.z - clip_off.x) * scale_x;
      double y1 = (cmd.ClipRect.w - clip_off.y) * scale_y;
      x0 = std::max(0.0, std::min(x0, (double)target_width));
      y0 = std::max(0.0, std::min(y0, (double)target_height));
      x1 = std::max(0.0, std::min(x1, (double)target_width));
      y1 = std::max(0.0, std::min(y1, (double)target_height));
      if (x1 <= x0 || y1 <= y0) {
        continue;
      }
      [encoder setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0,
                                               (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];

      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:cmd.ElemCount
                           indexType:sizeof(ImDrawIdx) == 2 ? MTLIndexTypeUInt16
                                                            : MTLIndexTypeUInt32
                         indexBuffer:ibuf
                   indexBufferOffset:(offsets[n].idx + cmd.IdxOffset) * sizeof(ImDrawIdx)
                       instanceCount:1
                          baseVertex:offsets[n].vtx + cmd.VtxOffset
                        baseInstance:0];
    }
  }

  // Leave the scissor covering the whole target: the next thing to use this encoder is not this.
  [encoder setScissorRect:(MTLScissorRect){0, 0, target_width, target_height}];
}
