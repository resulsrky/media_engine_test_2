#include "hydra/media/SdlRenderer.h"

#include <stdexcept>
#include <cstring>

extern "C" {
#include <SDL2/SDL.h>
}

namespace hydra::media {

SdlRenderer::SdlRenderer() {}
SdlRenderer::~SdlRenderer() {
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  SDL_Quit();
}

void SdlRenderer::open(int width, int height, const std::string& title) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    throw std::runtime_error("SDL_Init failed");
  }
  window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             width, height, SDL_WINDOW_RESIZABLE);
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
  tex_w_ = width; tex_h_ = height;
}

void SdlRenderer::render(const DecodedFrame& frame) {
  if (!texture_ || frame.width != tex_w_ || frame.height != tex_h_) {
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
    tex_w_ = frame.width; tex_h_ = frame.height;
  }
  // Update IYUV planes
  SDL_UpdateYUVTexture(texture_, nullptr,
                       frame.plane_y.data(), frame.width,
                       frame.plane_u.data(), frame.width/2,
                       frame.plane_v.data(), frame.width/2);
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void SdlRenderer::poll() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) {
      // no-op; caller can handle if needed
    }
  }
}

} // namespace hydra::media


