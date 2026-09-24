#pragma once

/*!
 * @file metal.h
 * Metal rendering backend for Apple Silicon.
 *
 * This is the scaffolding stage of the OpenGL -> Metal port: it owns the window, the MTLDevice,
 * the command queue and the CAMetalLayer, and presents a cleared frame. The bucket renderers
 * still live in game/graphics/opengl_renderer/ and are ported over one at a time.
 *
 * The OpenGL backend stays fully functional and is the reference this is validated against --
 * select between them at runtime with --renderer (see Gfx::GetRenderer).
 *
 * The implementation is Objective-C++ (metal.mm); this header stays plain C++ so the rest of the
 * runtime can include it.
 */

#include <memory>

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"

#include "third-party/SDL/include/SDL3/SDL.h"

// Opaque holder for the Objective-C objects (MTLDevice, MTLCommandQueue, CAMetalLayer).
// Declared here so metal.h needs no Objective-C types.
struct MetalContext;

class MetalDisplay : public GfxDisplay {
 public:
  MetalDisplay(SDL_Window* window, SDL_MetalView view, std::unique_ptr<MetalContext> ctx,
               bool is_main);
  ~MetalDisplay() override;

  std::shared_ptr<DisplayManager> get_display_manager() const override { return m_display_manager; }
  std::shared_ptr<InputManager> get_input_manager() const override { return m_input_manager; }

  void render() override;
  void init_splash() override;
  void draw_splash(int fb_w, int fb_h) override;

 private:
  void process_sdl_events();

  SDL_Window* m_window = nullptr;
  SDL_MetalView m_view = nullptr;
  std::unique_ptr<MetalContext> m_ctx;

  std::shared_ptr<DisplayManager> m_display_manager;
  std::shared_ptr<InputManager> m_input_manager;

  bool m_should_quit = false;
  // The common texture pack ("GAME") has to be in the pool before the game uploads anything that
  // references it. Loaded on the first frame, which is the first point the render thread owns.
  bool m_common_level_loaded = false;
};

extern const GfxRendererModule gRendererMetal;
