/*!
 * @file metal.mm
 * Metal rendering backend for Apple Silicon. See metal.h for how this fits into the port.
 */

#include "metal.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/system/hid/sdl_util.h"

// Holds the Objective-C objects so metal.h can stay plain C++.
struct MetalContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CAMetalLayer* layer = nil;
};

namespace {

bool g_metal_inited = false;

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
  g_metal_inited = true;

  return std::make_shared<MetalDisplay>(window, view, std::move(ctx), is_main);
}

u32 metal_vsync() {
  return 0;
}

u32 metal_sync_path() {
  return 0;
}

// The DMA chain still targets the OpenGL bucket renderers; nothing consumes it here yet.
void metal_send_chain(const void* /*data*/, u32 /*offset*/) {}
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
    // Bucket renderers get ported in here.
    [enc endEncoding];
    [cmd presentDrawable:drawable];
    [cmd commit];
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
