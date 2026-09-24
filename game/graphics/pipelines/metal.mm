/*!
 * @file metal.mm
 * Metal rendering backend for Apple Silicon. See metal.h for how this fits into the port.
 */

#include "metal.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <condition_variable>
#include <mutex>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"

#include "game/graphics/gfx.h"
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
  id<MTLRenderPipelineState> solid_color_pso = nil;
};

namespace {

bool g_metal_inited = false;

// Mirrors GraphicsData in the OpenGL backend: the game thread hands a DMA chain over here and
// waits, the render thread consumes it. Kept separate so the two backends cannot interfere.
struct MetalGraphicsData {
  explicit MetalGraphicsData(u32 main_memory_size) : dma_copier(main_memory_size) {}

  std::mutex sync_mutex;
  std::condition_variable sync_cv;

  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  FixedChunkDmaCopier dma_copier;
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
                                               GameVersion /*game_version*/,
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
  ctx->layer.framebufferOnly = YES;

  lg::info("[Metal] device: {}", [[ctx->device name] UTF8String]);

  // Compile the MSL shaders. Runtime compilation keeps the shader edit loop fast and avoids
  // requiring the Metal Toolchain; see MetalShaderLibrary.h.
  {
    std::string combined;
    try {
      for (const auto& def : metal_shaders::all_shaders()) {
        combined += metal_shaders::load_source_with_includes(def.file);
        combined += "\n";
      }
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

    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [ctx->library newFunctionWithName:@"solid_color_vert"];
    desc.fragmentFunction = [ctx->library newFunctionWithName:@"solid_color_frag"];
    desc.colorAttachments[0].pixelFormat = ctx->layer.pixelFormat;
    ctx->solid_color_pso = [ctx->device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!ctx->solid_color_pso) {
      lg::error("[Metal] solid_color pipeline failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return nullptr;
    }
  }
  if (!g_metal_gfx_data) {
    g_metal_gfx_data = std::make_unique<MetalGraphicsData>(EE_MAIN_MEM_SIZE);
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
void metal_texture_upload_now(const u8* /*tpage*/, int /*mode*/, u32 /*s7_ptr*/) {}
void metal_texture_relocate(u32 /*destination*/, u32 /*source*/, u32 /*format*/) {}
void metal_set_levels(const std::vector<std::string>& /*levels*/) {}
void metal_set_active_levels(const std::vector<std::string>& /*levels*/) {}
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

  if (have_frame) {
    // Same shape as OpenGLRenderer::dispatch_buckets_jak1: a call to the default-registers chain,
    // then one fixed-size slot per bucket. Each bucket slot is 16 bytes on from the last, and a
    // renderer is expected to consume exactly its own bucket.
    //
    // Nothing is rendered yet -- this walks the chain and skips each bucket's data so the frame
    // structure can be verified before a ported renderer depends on it. Bucket renderers get
    // dispatched from this loop.
    DmaFollower dma = dma_for_frame;
    u32 buckets_base = dma.current_tag_offset() + 16;  // 1 qw for the initial call
    u32 next_bucket = buckets_base;

    dma.read_and_advance();  // the call into the default-regs chain
    dma.read_and_advance();  // the default register data itself
    dma.read_and_advance();  // its ret tag

    int buckets_walked = 0;
    if (dma.current_tag_offset() == next_bucket) {
      next_bucket += 16;
      for (int bucket_id = 0; bucket_id < kMetalBucketCount; bucket_id++) {
        // Skip everything this bucket holds; a real renderer would consume it instead.
        while (dma.current_tag_offset() != next_bucket && !dma.ended()) {
          dma.read_and_advance();
        }
        buckets_walked++;
        if (dma.ended()) {
          break;
        }
        next_bucket += 16;
      }
    }

    static bool logged_first = false;
    if (!logged_first) {
      lg::info("[Metal] first frame from GOAL: walked {} of {} buckets", buckets_walked,
               kMetalBucketCount);
      logged_first = true;
    }
  }

  @autoreleasepool {
    id<CAMetalDrawable> drawable = [m_ctx->layer nextDrawable];
    if (!drawable) {
      // The layer can legitimately run out of drawables (e.g. the window is occluded).
      return;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Distinctive clear colour: proof the Metal path is what is on screen, not OpenGL.
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.25, 1.0);

    id<MTLCommandBuffer> cmd = [m_ctx->queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];

    // Smoke test for the shader path: draws a triangle with the ported solid_color shader.
    // It is the proof that source -> MTLLibrary -> pipeline state -> draw works before any
    // bucket renderer depends on it. Delete once tfrag3 draws here.
    if (m_ctx->solid_color_pso) {
      struct Vert2 {
        float x, y;
      };  // matches MSL float2
      const Vert2 verts[3] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f}};
      SolidColorUniforms uniforms{};
      uniforms.fragment_color = {1.0f, 0.4f, 0.1f, 1.0f};
      [enc setRenderPipelineState:m_ctx->solid_color_pso];
      [enc setVertexBytes:verts length:sizeof(verts) atIndex:MetalBufferIndexVertex];
      [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }

    // Bucket renderers get ported in here.
    [enc endEncoding];
    [cmd presentDrawable:drawable];
    [cmd commit];
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
