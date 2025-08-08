#pragma once

#include <functional>
#include <memory>
#include <string>

#include "hydra/media/EncodedFrame.h"

namespace hydra::media {

class MediaPipeline {
public:
  using EncodedFrameCallback = std::function<void(const EncodedFrame&)>;
  virtual ~MediaPipeline() = default;
  virtual void start(EncodedFrameCallback callback) = 0;
  virtual void stop() = 0;
};

} // namespace hydra::media


