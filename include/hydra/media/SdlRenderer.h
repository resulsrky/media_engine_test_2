#pragma once

#include <string>

#include "hydra/media/DecodedFrame.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace hydra::media {

class SdlRenderer {
public:
  SdlRenderer();
  ~SdlRenderer();

  void open(int width, int height, const std::string& title);
  void render(const DecodedFrame& frame);
  void poll();

private:
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
  SDL_Texture* texture_{nullptr};
  int tex_w_{0};
  int tex_h_{0};
};

} // namespace hydra::media


