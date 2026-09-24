/*!
 * @file metal.mm
 * Metal rendering backend for Apple Silicon. See metal.h for how this fits into the port.
 */

#include "metal.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <condition_variable>
#include <mutex>

#include <cstdlib>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"

#include "common/util/FileUtil.h"


#include "game/graphics/gfx.h"
#include "game/runtime.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"
#include "game/graphics/metal_renderer/MetalRenderer.h"
#include "game/graphics/metal_renderer/MetalShaderLibrary.h"
#include "game/system/hid/sdl_util.h"

#include "metal_shader_types.h"

// Holds the Objective-C objects so metal.h can stay plain C++.
struct MetalContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CAMetalLayer* layer = nil;
  id<MTLLibrary> library = nil;
  // First ported shader. Proves source -> MTLLibrary -> pipeline state -> draw end to end.
  // Depth buffer for the world geometry. Recreated whenever the drawable changes size.
  id<MTLTexture> frame_texture = nil;
  id<MTLTexture> depth_texture = nil;
  u32 depth_w = 0, depth_h = 0;
};

// Pixel formats the render pass and every pipeline state must agree on.
constexpr MTLPixelFormat kMetalColorFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kMetalDepthFormat = MTLPixelFormatDepth32Float;

// See kMetalFramesInFlight in MetalRenderState.h for what this is holding back and why.
dispatch_semaphore_t g_metal_frame_semaphore = dispatch_semaphore_create(kMetalFramesInFlight);

namespace {

bool g_metal_inited = false;

// Mirrors GraphicsData in the OpenGL backend: the game thread hands a DMA chain over here and
// waits, the render thread consumes it. Kept separate so the two backends cannot interfere.
constexpr PerGameVersion<int> metal_fr3_level_count(jak1::LEVEL_TOTAL,
                                                    jak2::LEVEL_TOTAL,
                                                    jak3::LEVEL_TOTAL,
                                                    jakx::LEVEL_TOTAL);

struct MetalGraphicsData {
  MetalGraphicsData(u32 main_memory_size, GameVersion version)
      : dma_copier(main_memory_size),
        texture_pool(std::make_shared<TexturePool>(version)),
        loader(std::make_shared<Loader>(
            file_util::get_jak_project_dir() / "out" / game_version_names[version] / "fr3",
            metal_fr3_level_count[version])),
        version(version) {
    renderer = std::make_unique<MetalRenderer>(texture_pool, loader);
  }

  std::mutex sync_mutex;
  std::condition_variable sync_cv;

  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  FixedChunkDmaCopier dma_copier;

  // Shared with the OpenGL backend: the texture conversion and the .fr3 level data are identical,
  // and both allocate through gpu::, which metal_make_display has pointed at Metal by the time
  // anything here runs.
  std::shared_ptr<TexturePool> texture_pool;
  std::shared_ptr<Loader> loader;
  GameVersion version;

  // The bucket table: one entry per bucket, empty where a renderer is not ported yet.
  std::unique_ptr<MetalRenderer> renderer;
  bool renderer_ready = false;
};

std::unique_ptr<MetalGraphicsData> g_metal_gfx_data;

// Matches the OpenGL backend: the renderers read the game's DMA buffer directly rather than a
// copy. Flip to true to get a snapshot that survives a corrupt buffer.
constexpr bool metal_run_dma_copy = false;

// Jak 1 bucket count. Checked against the real chain at runtime; the log line reports how many
// were actually walked, so a mismatch is visible rather than silent.
constexpr int kMetalBucketCount = (int)jak1::BucketId::MAX_BUCKETS;

int metal_init(GfxGlobalSettings& /*settings*/) {
  prof().instant_event("ROOT");
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    sdl_util::log_error("Could not initialize SDL, exiting");
    return 1;
  }
  lg::info("[Metal] SDL initialized, compiled with {} | linked with {}", SDL_VERSION,
           SDL_GetVersion());
  return 0;
}

void metal_exit() {
  g_metal_gfx_data.reset();
  g_metal_inited = false;
}

std::shared_ptr<GfxDisplay> metal_make_display(int width,
                                               int height,
                                               const char* title,
                                               GfxGlobalSettings& /*settings*/,
                                               GameVersion game_version,
                                               bool is_main) {
  SDL_Window* window = SDL_CreateWindow(
      title, width, height,
      SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    sdl_util::log_error("metal_make_display failed - could not create window");
    return nullptr;
  }

  // SDL creates the NSView backed by a CAMetalLayer for us.
  SDL_MetalView view = SDL_Metal_CreateView(window);
  if (!view) {
    sdl_util::log_error("metal_make_display failed - could not create Metal view");
    SDL_DestroyWindow(window);
    return nullptr;
  }

  auto ctx = std::make_unique<MetalContext>();
  ctx->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
  ctx->device = MTLCreateSystemDefaultDevice();
  if (!ctx->device) {
    lg::error("[Metal] no Metal device available");
    SDL_Metal_DestroyView(view);
    SDL_DestroyWindow(window);
    return nullptr;
  }
  ctx->queue = [ctx->device newCommandQueue];
  if (!ctx->queue) {
    lg::error("[Metal] could not create command queue");
    SDL_Metal_DestroyView(view);
    SDL_DestroyWindow(window);
    return nullptr;
  }

  ctx->layer.device = ctx->device;
  ctx->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  // The renderer draws every frame, so no need to keep the previous drawable contents.
  // NO, because the finished frame is blitted into the drawable rather than rendered into it,
  // and the debug screenshot reads it back.
  ctx->layer.framebufferOnly = NO;

  lg::info("[Metal] device: {}", [[ctx->device name] UTF8String]);

  // Route every backend-neutral GPU resource creation (TexturePool, the loader stages) to Metal
  // instead of OpenGL. Must happen before anything uploads a texture.
  metal_install_gpu_resource_backend((__bridge void*)ctx->device, (__bridge void*)ctx->queue);

  // Compile the MSL shaders. Runtime compilation keeps the shader edit loop fast and avoids
  // requiring the Metal Toolchain; see MetalShaderLibrary.h.
  {
    std::string combined;
    try {
      combined = metal_shaders::load_all_sources();
    } catch (const std::exception& e) {
      lg::error("[Metal] could not read shader sources: {}", e.what());
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return nullptr;
    }

    NSError* err = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    ctx->library = [ctx->device newLibraryWithSource:[NSString stringWithUTF8String:combined.c_str()]
                                             options:opts
                                               error:&err];
    if (!ctx->library) {
      lg::error("[Metal] shader compilation failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return nullptr;
    }
    lg::info("[Metal] compiled {} shader file(s)", metal_shaders::all_shaders().size());

  }
  if (!g_metal_gfx_data) {
    g_metal_gfx_data = std::make_unique<MetalGraphicsData>(EE_MAIN_MEM_SIZE, game_version);
  }
  g_metal_inited = true;

  return std::make_shared<MetalDisplay>(window, view, std::move(ctx), is_main);
}

u32 metal_vsync() {
  if (!g_metal_gfx_data) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_metal_gfx_data->sync_mutex);
  auto init_frame = g_metal_gfx_data->frame_idx_of_input_data;
  g_metal_gfx_data->sync_cv.wait(lock, [=] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || g_metal_gfx_data->frame_idx > init_frame;
  });
  return g_metal_gfx_data->frame_idx & 1;
}

u32 metal_sync_path() {
  if (!g_metal_gfx_data) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_metal_gfx_data->sync_mutex);
  if (!g_metal_gfx_data->has_data_to_render) {
    return 0;
  }
  g_metal_gfx_data->sync_cv.wait(lock,
                                 [=] { return !g_metal_gfx_data->has_data_to_render; });
  return 0;
}

/*!
 * Hand a DMA chain to the render thread. Called from the game thread, on a GOAL stack.
 */
void metal_send_chain(const void* data, u32 offset) {
  if (!g_metal_gfx_data) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_metal_gfx_data->dma_mutex);
  if (g_metal_gfx_data->has_data_to_render) {
    lg::error("[Metal] send_chain called while a frame is still pending");
    return;
  }
  g_metal_gfx_data->dma_copier.set_input_data(data, offset, metal_run_dma_copy);
  g_metal_gfx_data->frame_idx_of_input_data = g_metal_gfx_data->frame_idx;
  g_metal_gfx_data->has_data_to_render = true;
  g_metal_gfx_data->dma_cv.notify_all();
}
void metal_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  if (g_metal_gfx_data) {
    g_metal_gfx_data->texture_pool->handle_upload_now(tpage, mode, g_ee_main_mem, s7_ptr, false);
  }
}

void metal_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_metal_gfx_data) {
    g_metal_gfx_data->texture_pool->relocate(destination, source, format);
  }
}

void metal_set_levels(const std::vector<std::string>& levels) {
  if (g_metal_gfx_data) {
    g_metal_gfx_data->loader->set_want_levels(levels);
  }
}

void metal_set_active_levels(const std::vector<std::string>& levels) {
  if (g_metal_gfx_data) {
    g_metal_gfx_data->loader->set_active_levels(levels);
  }
}
void metal_set_pmode_alp(float /*val*/) {}

}  // namespace

MetalDisplay::MetalDisplay(SDL_Window* window,
                           SDL_MetalView view,
                           std::unique_ptr<MetalContext> ctx,
                           bool is_main)
    : m_window(window),
      m_view(view),
      m_ctx(std::move(ctx)),
      m_display_manager(std::make_shared<DisplayManager>(window)),
      m_input_manager(std::make_shared<InputManager>(window)) {
  m_main = is_main;
  m_display_manager->set_input_manager(m_input_manager);
}

MetalDisplay::~MetalDisplay() {
  metal_shutdown_gpu_resource_backend();
  if (m_ctx) {
    m_ctx->queue = nil;
    m_ctx->device = nil;
    m_ctx->layer = nil;
  }
  if (m_view) {
    SDL_Metal_DestroyView(m_view);
  }
  if (m_window) {
    SDL_DestroyWindow(m_window);
  }
  if (m_main) {
    metal_exit();
  }
}

void MetalDisplay::process_sdl_events() {
  SDL_Event evt;
  while (SDL_PollEvent(&evt)) {
    if (evt.type == SDL_EVENT_QUIT) {
      m_should_quit = true;
    }
    m_display_manager->process_sdl_event(evt);
    m_input_manager->process_sdl_event(evt);
  }
}

void MetalDisplay::render() {
  if (!m_common_level_loaded && g_metal_gfx_data) {
    auto p = scoped_prof("load-common");
    const auto& common =
        g_metal_gfx_data->loader->load_common(*g_metal_gfx_data->texture_pool, "GAME");
    lg::info("[Metal] common level loaded: {} textures", common.textures.size());
    m_common_level_loaded = true;
  }
  {
    auto p = scoped_prof("sdl-input-monitor-poll-for-kb-mouse");
    m_input_manager->poll_keyboard_data();
    m_input_manager->poll_mouse_data();
    m_input_manager->finish_polling();
  }
  process_sdl_events();
  {
    auto p = scoped_prof("display-manager-ee-events");
    m_display_manager->process_ee_events();
  }

  // Keep the drawable the same size as the window's backing store.
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(m_window, &w, &h);
  if (w > 0 && h > 0) {
    m_ctx->layer.drawableSize = CGSizeMake(w, h);
  }

  // Take the frame the game thread handed us, if there is one. The bucket loop below is where
  // the ported renderers will be dispatched from; right now it only walks the chain so the data
  // path can be verified before anything depends on it.
  bool have_frame = false;
  DmaFollower dma_for_frame(nullptr, 0);
  {
    std::unique_lock<std::mutex> lock(g_metal_gfx_data->dma_mutex);
    if (g_metal_gfx_data->has_data_to_render) {
      have_frame = true;
      if constexpr (metal_run_dma_copy) {
        const auto& chain = g_metal_gfx_data->dma_copier.get_last_result();
        dma_for_frame = DmaFollower(chain.data.data(), chain.start_offset);
      } else {
        dma_for_frame = DmaFollower(g_metal_gfx_data->dma_copier.get_last_input_data(),
                                    g_metal_gfx_data->dma_copier.get_last_input_offset());
      }
    }
  }

  // The frame's DMA chain is walked by MetalRenderer, inside the render pass below, so the
  // buckets draw where they are dispatched.

  // Pump the level loader. It reads the .fr3 files on its own thread and does the GPU-side work
  // here, through gpu::, which is pointed at Metal. Same call the OpenGL backend makes once per
  // frame.
  {
    auto p = scoped_prof("loader");
    g_metal_gfx_data->loader->update(*g_metal_gfx_data->texture_pool);
  }

  {
    static u64 logged_levels = 0;
    auto in_use = g_metal_gfx_data->loader->get_in_use_levels();
    if (in_use.size() != logged_levels) {
      logged_levels = in_use.size();
      std::string names;
      for (auto* lev : in_use) {
        names += lev->level->level_name + " ";
      }
      lg::info("[Metal] levels loaded: {}({})", logged_levels, names);
    }
  }

  // Hold the CPU back so it is never more than kMetalFramesInFlight frames ahead of the GPU.
  // Every renderer's per-frame buffers are sized for that many frames: without this the CPU runs
  // ahead and rewrites a buffer the GPU is still reading, which shows up as a flicker over the
  // whole picture rather than as an error.
  dispatch_semaphore_wait(g_metal_frame_semaphore, DISPATCH_TIME_FOREVER);

  @autoreleasepool {
    id<CAMetalDrawable> drawable = [m_ctx->layer nextDrawable];
    if (!drawable) {
      // The layer can legitimately run out of drawables (e.g. the window is occluded).
      dispatch_semaphore_signal(g_metal_frame_semaphore);
      return;
    }

    // Depth buffer, sized to the drawable. The world geometry needs one; the clear-and-present
    // frame did not.
    const u32 dw = (u32)drawable.texture.width;
    const u32 dh = (u32)drawable.texture.height;
    // The frame is drawn into a texture we own rather than straight into the drawable, and that
    // texture is blitted to the drawable afterwards. This display loop runs more often than the
    // game produces frames; drawing into the drawable would clear it on every one of those calls,
    // so a frame with no new data would come out blank -- a flicker over the whole picture.
    // Keeping the last frame in a texture of our own means those calls re-present it instead.
    if (!m_ctx->frame_texture || m_ctx->depth_w != dw || m_ctx->depth_h != dh) {
      MTLTextureDescriptor* fd =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kMetalColorFormat
                                                             width:dw
                                                            height:dh
                                                         mipmapped:NO];
      fd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      fd.storageMode = MTLStorageModePrivate;
      m_ctx->frame_texture = [m_ctx->device newTextureWithDescriptor:fd];
    }
    if (!m_ctx->depth_texture || m_ctx->depth_w != dw || m_ctx->depth_h != dh) {
      MTLTextureDescriptor* dd =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kMetalDepthFormat
                                                             width:dw
                                                            height:dh
                                                         mipmapped:NO];
      dd.usage = MTLTextureUsageRenderTarget;
      dd.storageMode = MTLStorageModePrivate;
      m_ctx->depth_texture = [m_ctx->device newTextureWithDescriptor:dd];
      m_ctx->depth_w = dw;
      m_ctx->depth_h = dh;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = m_ctx->frame_texture;
    // Only a real frame clears. Without one there is nothing to draw, and the texture already
    // holds the last frame.
    pass.colorAttachments[0].loadAction =
        have_frame ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Distinctive clear colour: proof the Metal path is what is on screen, not OpenGL.
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.25, 1.0);
    pass.depthAttachment.texture = m_ctx->depth_texture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    // The game's projection makes nearer geometry compare greater, so the far value is 0.
    pass.depthAttachment.clearDepth = 0.0;

    // Offscreen work goes in its own command buffer, committed first. See offscreen_cmd in
    // MetalRenderState.h.
    id<MTLCommandBuffer> offscreen_cmd = [m_ctx->queue commandBuffer];
    id<MTLCommandBuffer> cmd = [m_ctx->queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];

    // Every bucket, in order, through the bucket table.
    if (g_metal_gfx_data && have_frame) {
      if (!g_metal_gfx_data->renderer_ready) {
        g_metal_gfx_data->renderer_ready = g_metal_gfx_data->renderer->init(
            m_ctx->device, m_ctx->library, kMetalColorFormat, kMetalDepthFormat);
      }
      if (g_metal_gfx_data->renderer_ready) {
        g_metal_gfx_data->renderer->render(dma_for_frame, enc, offscreen_cmd, dw, dh);
        static u32 logged_tris = 0;
        if (g_metal_gfx_data->renderer->last_frame_tris() != logged_tris) {
          logged_tris = g_metal_gfx_data->renderer->last_frame_tris();
          lg::info("[Metal] drew {} triangles", logged_tris);
        }
      }
    }

    [enc endEncoding];
    [offscreen_cmd commit];

    // Hand the finished frame to the drawable.
    {
      id<MTLBlitCommandEncoder> present_blit = [cmd blitCommandEncoder];
      [present_blit copyFromTexture:m_ctx->frame_texture
                        sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(dw, dh, 1)
                          toTexture:drawable.texture
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
      [present_blit endEncoding];
    }

    // Debug readback: OPENGOAL_METAL_SCREENSHOT=<path> writes the first fully-drawn frame to a
    // PNG and stops. Metal draws into a drawable the window server owns, so there is no way to
    // capture this from outside the process while the window is on another Space -- and looking
    // at the frame is the only way to tell a wrong matrix from a wrong texture.
    static bool screenshot_done = false;
    const char* screenshot_path = std::getenv("OPENGOAL_METAL_SCREENSHOT");
    // Same trigger as the OpenGL backend: N frames after the named level is in use.
    const char* level_env = std::getenv("OPENGOAL_SCREENSHOT_LEVEL");
    const char* delay_env = std::getenv("OPENGOAL_SCREENSHOT_DELAY");
    const int want_delay = delay_env ? atoi(delay_env) : 0;
    // A frame during a fade or a menu draws a handful of triangles and is not worth capturing.
    // OPENGOAL_SCREENSHOT_MIN_TRIS says how much of a frame has to be there.
    const char* min_tris_env = std::getenv("OPENGOAL_SCREENSHOT_MIN_TRIS");
    const u32 shot_min_tris = min_tris_env ? (u32)atoi(min_tris_env) : 0;
    static int frames_since_level = -1;
    if (screenshot_path && !screenshot_done && g_metal_gfx_data) {
      for (const auto* lev : g_metal_gfx_data->loader->get_in_use_levels()) {
        if (!level_env || lev->level->level_name == level_env) {
          if (frames_since_level < 0) {
            frames_since_level = 0;
          }
          break;
        }
      }
      if (frames_since_level >= 0) {
        frames_since_level++;
      }
    }
    const bool want_screenshot = screenshot_path && !screenshot_done && have_frame &&
                                 g_metal_gfx_data &&
                                 frames_since_level >= want_delay && frames_since_level >= 0 &&
                                 g_metal_gfx_data->renderer->last_frame_tris() > shot_min_tris;
    id<MTLBuffer> screenshot_buffer = nil;
    u32 shot_w = 0, shot_h = 0, shot_stride = 0;
    if (want_screenshot) {
      shot_w = (u32)drawable.texture.width;
      shot_h = (u32)drawable.texture.height;
      shot_stride = shot_w * 4;
      screenshot_buffer = [m_ctx->device newBufferWithLength:shot_stride * shot_h
                                                     options:MTLResourceStorageModeShared];
      id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
      [blit copyFromTexture:drawable.texture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(shot_w, shot_h, 1)
                   toBuffer:screenshot_buffer
          destinationOffset:0
     destinationBytesPerRow:shot_stride
   destinationBytesPerImage:shot_stride * shot_h];
      [blit endEncoding];
    }
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(g_metal_frame_semaphore);
    }];
    [cmd presentDrawable:drawable];
    [cmd commit];

    if (screenshot_buffer) {
      [cmd waitUntilCompleted];
      // BGRA8 from the drawable; write_rgba_png wants RGBA.
      std::vector<u8> rgba(shot_stride * shot_h);
      const u8* src = (const u8*)[screenshot_buffer contents];
      for (u32 i = 0; i < shot_w * shot_h; i++) {
        rgba[i * 4 + 0] = src[i * 4 + 2];
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + 0];
        rgba[i * 4 + 3] = 255;
      }
      file_util::write_rgba_png(screenshot_path, rgba.data(), shot_w, shot_h);
      lg::info("[Metal] wrote screenshot {} ({}x{}) at frame {}", screenshot_path, shot_w, shot_h,
               g_metal_gfx_data->frame_idx);
      screenshot_done = true;
    }
  }

  // Release the game thread: it blocks in metal_sync_path/metal_vsync until the frame is done.
  if (have_frame) {
    std::unique_lock<std::mutex> lock(g_metal_gfx_data->dma_mutex);
    g_metal_gfx_data->has_data_to_render = false;
  }
  {
    std::unique_lock<std::mutex> lock(g_metal_gfx_data->sync_mutex);
    g_metal_gfx_data->frame_idx++;
    g_metal_gfx_data->sync_cv.notify_all();
  }

  if (m_should_quit) {
    MasterExit = RuntimeExitStatus::EXIT;
  }
}

void MetalDisplay::init_splash() {}

void MetalDisplay::draw_splash(int /*fb_w*/, int /*fb_h*/) {}

const GfxRendererModule gRendererMetal = {
    metal_init,                // init
    metal_make_display,        // make_display
    metal_exit,                // exit
    metal_vsync,               // vsync
    metal_sync_path,           // sync_path
    metal_send_chain,          // send_chain
    metal_texture_upload_now,  // texture_upload_now
    metal_texture_relocate,    // texture_relocate
    metal_set_levels,          // set_levels
    metal_set_active_levels,   // set_active_levels
    metal_set_pmode_alp,       // set_pmode_alp
    GfxPipeline::Metal,        // pipeline
    "Metal"                    // name
};
