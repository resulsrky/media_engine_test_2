#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "hydra/media/MediaPipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace hydra::media {

// Minimal, demo-level CPU encoder generating synthetic frames (color bars)
class FFmpegCpuEncodingPipeline final : public MediaPipeline {
public:
  explicit FFmpegCpuEncodingPipeline(int width, int height, int fps);
  ~FFmpegCpuEncodingPipeline() override;

  void start(EncodedFrameCallback callback) override;
  void stop() override;

private:
  void encode_loop();

  int width_{};
  int height_{};
  int fps_{};

  AVCodecContext* codec_ctx_{nullptr};
  AVFrame* frame_{nullptr};
  AVPacket* pkt_{nullptr};

  std::atomic<bool> running_{false};
  std::jthread worker_{};
  EncodedFrameCallback callback_{};
  std::uint64_t frame_id_{0};
};

} // namespace hydra::media


