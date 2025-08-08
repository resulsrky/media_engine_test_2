#pragma once

#include <functional>
#include <memory>

#include "hydra/media/EncodedFrame.h"
#include "hydra/media/DecodedFrame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace hydra::media {

class FFmpegDecoder {
public:
  using DecodedCallback = std::function<void(const DecodedFrame&)>;
  FFmpegDecoder();
  ~FFmpegDecoder();

  void init_h264(int width_hint, int height_hint);
  void push(const EncodedFrame& frame, DecodedCallback cb);

private:
  AVCodecContext* codec_ctx_{nullptr};
  AVFrame* frame_{nullptr};
  AVFrame* yuv420_{nullptr};
  AVPacket* pkt_{nullptr};
  SwsContext* sws_{nullptr};
};

} // namespace hydra::media


