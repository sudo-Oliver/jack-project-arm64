/*!
 * @file MetalTextureUploadHandler.mm
 * See MetalTextureUploadHandler.h.
 */

#include "MetalTextureUploadHandler.h"

#include "common/log/log.h"

void MetalTextureUploadHandler::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_upload_count = 0;
  std::vector<TextureUpload> uploads;

  while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
    auto dma_tag = dma.current_tag();
    auto vif0 = dma.current_tag_vifcode0();

    // Immediate 12 is the texture animator's data. The animator is not ported yet, but the
    // uploads queued before it must still land first: that ordering is what makes an upload and
    // an animation to the same page come out in the order the game asked for.
    if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate == 12) {
      flush_uploads(uploads, render_state);
      uploads.clear();
    }

    auto data = dma.read_and_advance();
    if (data.size_bytes == 0 && data.vif0() == 0 && data.vif1() == 0) {
      continue;
    }

    // One upload record: which page of EE memory, and in which PS2 texture format.
    if (data.size_bytes == 16 && data.vifcode0().kind == VifCode::Kind::PC_PORT &&
        data.vif1() == 3) {
      TextureUpload upload_data;
      memcpy(&upload_data, data.data, sizeof(upload_data));
      uploads.push_back(upload_data);
      continue;
    }

    // A call chain ends the bucket: step over call, cnt and ret, which lands on the next bucket.
    if (dma_tag.kind == DmaTag::Kind::CALL) {
      dma.read_and_advance();
      dma.read_and_advance();
      dma.read_and_advance();
    }
  }

  flush_uploads(uploads, render_state);
}

void MetalTextureUploadHandler::flush_uploads(std::vector<TextureUpload>& uploads,
                                              MetalRenderState* render_state) {
  if (!render_state->ee_main_memory) {
    return;
  }
  m_upload_count += uploads.size();
  const u8* ee_mem = (const u8*)render_state->ee_main_memory;
  for (auto& upload : uploads) {
    render_state->texture_pool->handle_upload_now(ee_mem + upload.page, upload.mode, ee_mem,
                                                  render_state->offset_of_s7, false);
  }
}
