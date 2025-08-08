#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "hydra/media/MediaPipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

namespace hydra::media {

class FFmpegCameraEncodingPipeline final : public MediaPipeline {
public:
  FFmpegCameraEncodingPipeline(std::string device, int width, int height, int fps);
  ~FFmpegCameraEncodingPipeline() override;

  void start(EncodedFrameCallback callback) override;
  void stop() override;

private:
  void loop();

  std::string device_;
  int width_{};
  int height_{};
  int fps_{};

  AVFormatContext* fmt_ctx_{nullptr};
  AVCodecContext* enc_ctx_{nullptr};
  AVFrame* frame_{nullptr};
  AVPacket* pkt_{nullptr};
  SwsContext* sws_{nullptr};
  int video_stream_index_{-1};

  std::atomic<bool> running_{false};
  std::jthread worker_{};
  EncodedFrameCallback callback_{};
  std::uint64_t frame_id_{0};
};

} // namespace hydra::media


