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
#include "game/graphics/opengl_renderer/debug_gui.h"
#include "common/global_profiler/GlobalProfiler.h"
#include <atomic>

#include "common/util/Timer.h"

#include "common/log/log.h"

#include "common/util/FileUtil.h"


#include "game/graphics/gfx.h"
#include "game/runtime.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"
#include "game/graphics/metal_renderer/MetalImGui.h"
#include "game/graphics/metal_renderer/MetalRenderer.h"
#include "game/graphics/metal_renderer/MetalShaderLibrary.h"
#include "game/system/hid/sdl_util.h"

#include "metal_shader_types.h"
#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_impl_sdl3.h"
#include "third-party/imgui/imgui_style.h"

// Holds the Objective-C objects so metal.h can stay plain C++.
struct MetalContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CAMetalLayer* layer = nil;
  id<MTLLibrary> library = nil;
  // First ported shader. Proves source -> MTLLibrary -> pipeline state -> draw end to end.
  // Depth buffer for the world geometry. Recreated whenever the drawable changes size.
  id<MTLTexture> depth_texture = nil;
  u32 depth_w = 0, depth_h = 0;

  // The game is drawn into this rather than straight into the drawable, and copied onto the
  // drawable at the end of the frame. Two renderers -- the depth cue and the sprite distorter --
  // have to sample the frame they are drawing into, and Metal cannot sample a drawable that the
  // window server owns. `snapshot_texture` is where that sample comes from.
  id<MTLTexture> scene_texture = nil;
  id<MTLTexture> snapshot_texture = nil;
  u32 scene_w = 0, scene_h = 0;

  id<MTLRenderPipelineState> present_pso = nil;
  id<MTLSamplerState> present_sampler = nil;
};

// Pixel formats the render pass and every pipeline state must agree on.
constexpr MTLPixelFormat kMetalColorFormat = MTLPixelFormatBGRA8Unorm;
// Depth *and* stencil: the shadow renderer draws its volumes into the stencil buffer, and on
// Apple GPUs a combined format is one tile allocation rather than two.
constexpr MTLPixelFormat kMetalDepthFormat = MTLPixelFormatDepth32Float_Stencil8;

// See kMetalFramesInFlight in MetalRenderState.h for what this is holding back and why.
dispatch_semaphore_t g_metal_frame_semaphore = dispatch_semaphore_create(kMetalFramesInFlight);

namespace {

bool g_metal_inited = false;

// Filled in by the frame's completion handler, on a background thread, and read by the log line.
std::atomic<double> g_metal_gpu_seconds{0};
std::atomic<int> g_metal_gpu_samples{0};

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

  // The debug menu bar and its windows. Same class the OpenGL backend uses -- it has no graphics
  // API in it, only ImGui calls.
  OpenGlDebugGui debug_gui;
  std::string imgui_filename, imgui_log_filename;
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
// The PCRTC alpha register. Zero means the screen is blacked out, which is when the game loads a
// level, and coming out of that is the moment everything has to be ready.
std::atomic<float> g_metal_pmode_alp{1.f};
double g_metal_worst_frame_ms = 0;
void metal_set_pmode_alp(float val) {
  g_metal_pmode_alp = val;
}

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

  // The same key the OpenGL display binds, so the debug GUI is reached the same way on both.
  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(Gfx::g_debug_settings.hide_imgui_key, [&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
          if (!Gfx::g_debug_settings.ignore_hide_imgui) {
            set_imgui_visible(!is_imgui_visible());
          }
        }
      }));
}

void MetalDisplay::init_imgui() {
  // Same setup the OpenGL display does, minus its renderer backend: the context, the settings
  // files, the style, and ImGui's SDL3 platform backend.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  g_metal_gfx_data->imgui_filename = file_util::get_file_path({"imgui.ini"});
  g_metal_gfx_data->imgui_log_filename = file_util::get_file_path({"imgui_log.txt"});
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  io.IniFilename = g_metal_gfx_data->imgui_filename.c_str();
  io.LogFilename = g_metal_gfx_data->imgui_log_filename.c_str();

  if (Gfx::g_debug_settings.alternate_style) {
    ImGui::applyAlternateStyle();
  }
  ImGui::applyFontStyle();

  ImGui_ImplSDL3_InitForMetal(m_window);
  // imgui's setup calls functions that may fail intentionally and leaves the error set.
  SDL_ClearError();

  // The renderer has to exist before the first NewFrame: ImGui builds its font atlas through the
  // renderer backend, and NewFrame reads the built font. Doing it lazily on the first visible
  // frame crashes in ImGui::Begin.
  m_imgui = std::make_unique<MetalImGui>();
  m_imgui_renderer_ready = m_imgui->init(m_ctx->device, m_ctx->library, kMetalColorFormat);
  if (!m_imgui_renderer_ready) {
    lg::error("[Metal] debug GUI failed to initialise; it will not be drawn");
  }
  m_imgui_inited = true;
  set_imgui_visible(Gfx::g_debug_settings.show_imgui);
  g_metal_gfx_data->debug_gui.master_enable = Gfx::g_debug_settings.show_imgui;
}

MetalDisplay::~MetalDisplay() {
  if (m_imgui_inited) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_imgui_inited = false;
  }
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
    if (m_imgui_inited) {
      ImGui_ImplSDL3_ProcessEvent(&evt);
    }
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
  if (!m_imgui_inited) {
    auto p = scoped_prof("startup::metal::init_imgui");
    init_imgui();
  }
  process_sdl_events();
  {
    auto p = scoped_prof("display-manager-ee-events");
    m_display_manager->process_ee_events();
  }

  // The debug GUI's frame. It has to be opened and closed on every frame this function runs,
  // including the ones that draw nothing, or ImGui asserts on the next NewFrame.
  {
    auto p = scoped_prof("imgui-new-frame");
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
  }
  g_metal_gfx_data->debug_gui.master_enable = is_imgui_visible();
  if (is_imgui_visible()) {
    auto p = scoped_prof("debug-gui");
    g_metal_gfx_data->debug_gui.draw(g_metal_gfx_data->dma_copier.get_last_result().stats);
  }
  ImGui::Render();

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
    // During a blackout the screen shows nothing, so the loader can take as long as it likes.
    // Coming out of one without having finished is what leaves a level untextured and grey for
    // its first seconds, with the textures trickling in afterwards.
    static float last_pmode_alp = 1.f;
    const float pmode_alp = g_metal_pmode_alp;
    if (last_pmode_alp == 0.f && pmode_alp != 0.f) {
      g_metal_gfx_data->loader->update_blocking(*g_metal_gfx_data->texture_pool);
    } else {
      g_metal_gfx_data->loader->update(*g_metal_gfx_data->texture_pool);
    }
    last_pmode_alp = pmode_alp;
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
  //
  // This whole block is skipped when the game thread has no frame for us. The layer goes on
  // showing the drawable it was last given, so there is nothing to redraw -- and nothing to
  // clear, which is what used to put a blank frame between every real one. The bookkeeping below
  // it still runs: the game thread is waiting on it.
  if (have_frame) {
  dispatch_semaphore_wait(g_metal_frame_semaphore, DISPATCH_TIME_FOREVER);

  @autoreleasepool {
    // A lambda, so that giving up on this frame leaves the bookkeeping below it to run: the game
    // thread is waiting on that.
    [&] {
    id<CAMetalDrawable> drawable = [m_ctx->layer nextDrawable];
    if (!drawable) {
      // The layer can legitimately run out of drawables (e.g. the window is occluded).
      dispatch_semaphore_signal(g_metal_frame_semaphore);
      return;
    }

    // The game is drawn at the resolution the game's own settings ask for, and the final pass
    // scales it onto the drawable -- the same thing the OpenGL backend does with its render FBO
    // and the blit out of it. Rendering at the drawable's size instead would ignore the setting,
    // and on a Retina display that is four times the pixels for no one's benefit.
    const u32 window_w = (u32)drawable.texture.width;
    const u32 window_h = (u32)drawable.texture.height;
    u32 dw = (u32)Gfx::g_global_settings.game_res_w;
    u32 dh = (u32)Gfx::g_global_settings.game_res_h;
    if (dw == 0 || dh == 0) {
      dw = window_w;
      dh = window_h;
    }
    if (!m_ctx->scene_texture || m_ctx->scene_w != dw || m_ctx->scene_h != dh) {
      MTLTextureDescriptor* sd =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kMetalColorFormat
                                                             width:dw
                                                            height:dh
                                                         mipmapped:NO];
      sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      sd.storageMode = MTLStorageModePrivate;
      m_ctx->scene_texture = [m_ctx->device newTextureWithDescriptor:sd];
      // Same size and format: the snapshot is a straight copy of the scene so far.
      sd.usage = MTLTextureUsageShaderRead;
      m_ctx->snapshot_texture = [m_ctx->device newTextureWithDescriptor:sd];
      m_ctx->scene_w = dw;
      m_ctx->scene_h = dh;
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

    // The vsync setting is the game's, the same one the OpenGL backend hands to
    // SDL_GL_SetSwapInterval. On a CAMetalLayer it is displaySyncEnabled, and turning it off is
    // what makes the frame rate measurable rather than pinned to the display.
    if (Gfx::g_global_settings.vsync != Gfx::g_global_settings.old_vsync) {
      Gfx::g_global_settings.old_vsync = Gfx::g_global_settings.vsync;
      m_ctx->layer.displaySyncEnabled = Gfx::g_global_settings.vsync ? YES : NO;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = m_ctx->scene_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Distinctive clear colour: proof the Metal path is what is on screen, not OpenGL.
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.25, 1.0);
    pass.depthAttachment.texture = m_ctx->depth_texture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    // The game's projection makes nearer geometry compare greater, so the far value is 0.
    pass.depthAttachment.clearDepth = 0.0;
    // The shadow volumes count into the stencil buffer and read the count back in the same frame,
    // so it starts at zero and never has to be stored.
    pass.stencilAttachment.texture = m_ctx->depth_texture;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
    pass.stencilAttachment.clearStencil = 0;

    // Offscreen work goes in its own command buffer, committed first. See offscreen_cmd in
    // MetalRenderState.h.
    id<MTLCommandBuffer> offscreen_cmd = [m_ctx->queue commandBuffer];
    id<MTLCommandBuffer> cmd = [m_ctx->queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];

    // Every bucket, in order, through the bucket table.
    if (g_metal_gfx_data) {
      if (!g_metal_gfx_data->renderer_ready) {
        g_metal_gfx_data->renderer_ready = g_metal_gfx_data->renderer->init(
            m_ctx->device, m_ctx->library, kMetalColorFormat, kMetalDepthFormat);
        if (g_metal_gfx_data->renderer_ready && !m_ctx->present_pso) {
          MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
          pd.vertexFunction = [m_ctx->library newFunctionWithName:@"present_vert"];
          pd.fragmentFunction = [m_ctx->library newFunctionWithName:@"present_frag"];
          pd.colorAttachments[0].pixelFormat = kMetalColorFormat;
          NSError* perr = nil;
          m_ctx->present_pso = [m_ctx->device newRenderPipelineStateWithDescriptor:pd
                                                                             error:&perr];
          if (!m_ctx->present_pso) {
            lg::error("[Metal] present pipeline failed: {}",
                      perr ? [[perr localizedDescription] UTF8String] : "unknown error");
          }
          MTLSamplerDescriptor* smp = [[MTLSamplerDescriptor alloc] init];
          smp.minFilter = MTLSamplerMinMagFilterLinear;
          smp.magFilter = MTLSamplerMinMagFilterLinear;
          smp.sAddressMode = MTLSamplerAddressModeClampToEdge;
          smp.tAddressMode = MTLSamplerAddressModeClampToEdge;
          m_ctx->present_sampler = [m_ctx->device newSamplerStateWithDescriptor:smp];
        }
      }
      if (g_metal_gfx_data->renderer_ready) {
        // How a renderer gets the frame so far as a texture: end this pass (keeping its colour,
        // depth and stencil), copy the colour out, and start a pass that loads all three back.
        // On an Apple GPU that is one tile store and one tile load, and it only happens on the
        // frames where a renderer actually asks.
        MetalRenderer::FrameHooks hooks;
        hooks.frame_cmd = cmd;
        hooks.pause_and_snapshot = [&](MetalRenderState*) -> id<MTLTexture> {
          [enc setColorStoreAction:MTLStoreActionStore atIndex:0];
          [enc setDepthStoreAction:MTLStoreActionStore];
          [enc setStencilStoreAction:MTLStoreActionStore];
          [enc endEncoding];

          id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
          [blit copyFromTexture:m_ctx->scene_texture
                    sourceSlice:0
                    sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(dw, dh, 1)
                      toTexture:m_ctx->snapshot_texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
          [blit endEncoding];
          return m_ctx->snapshot_texture;
        };
        hooks.resume = [&](MetalRenderState* rs) {
          MTLRenderPassDescriptor* resume = [MTLRenderPassDescriptor renderPassDescriptor];
          resume.colorAttachments[0].texture = m_ctx->scene_texture;
          resume.colorAttachments[0].loadAction = MTLLoadActionLoad;
          resume.colorAttachments[0].storeAction = MTLStoreActionStore;
          resume.depthAttachment.texture = m_ctx->depth_texture;
          resume.depthAttachment.loadAction = MTLLoadActionLoad;
          resume.depthAttachment.storeAction = MTLStoreActionDontCare;
          resume.stencilAttachment.texture = m_ctx->depth_texture;
          resume.stencilAttachment.loadAction = MTLLoadActionLoad;
          resume.stencilAttachment.storeAction = MTLStoreActionDontCare;
          enc = [cmd renderCommandEncoderWithDescriptor:resume];
          rs->encoder = enc;
        };
        enc = g_metal_gfx_data->renderer->render(dma_for_frame, enc, offscreen_cmd, dw, dh, hooks);
        static u32 logged_tris = 0;
        if (g_metal_gfx_data->renderer->last_frame_tris() != logged_tris) {
          logged_tris = g_metal_gfx_data->renderer->last_frame_tris();
          lg::info("[Metal] drew {} triangles", logged_tris);
        }
      }
    }

    [enc endEncoding];
    [offscreen_cmd commit];

    // Put the scene on screen. The OpenGL backend's equivalent is the blit out of its render FBO.
    if (m_ctx->present_pso) {
      MTLRenderPassDescriptor* present = [MTLRenderPassDescriptor renderPassDescriptor];
      present.colorAttachments[0].texture = drawable.texture;
      present.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      present.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> present_enc =
          [cmd renderCommandEncoderWithDescriptor:present];
      [present_enc setRenderPipelineState:m_ctx->present_pso];
      [present_enc setFragmentTexture:m_ctx->scene_texture atIndex:0];
      [present_enc setFragmentSamplerState:m_ctx->present_sampler atIndex:0];
      [present_enc setViewport:(MTLViewport){0.0, 0.0, (double)window_w, (double)window_h, 0.0,
                                            1.0}];
      [present_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

      // The debug GUI goes over the finished frame, at the window's resolution rather than the
      // game's: it is text, and it should be as sharp as the display allows.
      if (m_imgui_renderer_ready && is_imgui_visible()) {
        m_imgui->render(ImGui::GetDrawData(), present_enc, window_w, window_h);
      }
      [present_enc endEncoding];
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
      shot_w = window_w;
      shot_h = window_h;
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
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
      // How long the GPU was actually busy with this frame. The frame rate says nothing on its
      // own -- the GOAL engine paces itself to 60 -- so this is the number that compares the two
      // backends. The OpenGL backend measures the same thing with a timer query.
      const double gpu = [done GPUEndTime] - [done GPUStartTime];
      if (gpu > 0) {
        g_metal_gpu_seconds += gpu;
        g_metal_gpu_samples++;
      }
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
    }();
  }
  }  // if (have_frame)

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

  // OPENGOAL_FPS_LOG=1 prints the frame rate every second. Both backends do it the same way, so
  // the two numbers are comparable.
  {
    static Timer frame_timer;
    const double this_frame = frame_timer.getSeconds();
    frame_timer.start();
    if (this_frame > g_metal_worst_frame_ms) {
      g_metal_worst_frame_ms = this_frame;
    }
  }

  if (have_frame && std::getenv("OPENGOAL_FPS_LOG")) {
    static Timer fps_timer;
    static int fps_frames = 0;
    fps_frames++;
    if (fps_timer.getSeconds() >= 1.0) {
      lg::info(
          "[Metal] {:.1f} fps, worst frame {:.2f} ms, gpu {:.2f} ms, {} pipelines built ({:.1f} "
          "ms)",
          fps_frames / fps_timer.getSeconds(), g_metal_worst_frame_ms * 1000.0,
          g_metal_gpu_seconds * 1000.0 / std::max(1, g_metal_gpu_samples.load()),
          g_metal_pipeline_builds, g_metal_pipeline_build_seconds * 1000.0);
      g_metal_gpu_seconds = 0;
      g_metal_gpu_samples = 0;
      g_metal_pipeline_builds = 0;
      g_metal_pipeline_build_seconds = 0;
      fps_frames = 0;
      g_metal_worst_frame_ms = 0;
      fps_timer.start();
    }
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
