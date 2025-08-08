#include "hydra/media/FFmpegCpuPipeline.h"

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace hydra::media {

static AVCodecContext* create_h264_encoder(int width, int height, int fps) {
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec) throw std::runtime_error("H264 encoder not found");
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) throw std::runtime_error("Failed to alloc codec context");
  ctx->width = width;
  ctx->height = height;
  ctx->time_base = AVRational{1, fps};
  ctx->framerate = AVRational{fps, 1};
  ctx->gop_size = fps * 2;
  ctx->max_b_frames = 0;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    throw std::runtime_error("Failed to open H264 encoder");
  }
  return ctx;
}

FFmpegCpuEncodingPipeline::FFmpegCpuEncodingPipeline(int width, int height, int fps)
    : width_(width), height_(height), fps_(fps) {
  avcodec_register_all();
  codec_ctx_ = create_h264_encoder(width_, height_, fps_);

  frame_ = av_frame_alloc();
  frame_->format = codec_ctx_->pix_fmt;
  frame_->width = codec_ctx_->width;
  frame_->height = codec_ctx_->height;
  if (av_frame_get_buffer(frame_, 32) < 0) {
    throw std::runtime_error("Failed to alloc frame buffer");
  }

  pkt_ = av_packet_alloc();
}

FFmpegCpuEncodingPipeline::~FFmpegCpuEncodingPipeline() {
  stop();
  if (pkt_) av_packet_free(&pkt_);
  if (frame_) av_frame_free(&frame_);
  if (codec_ctx_) avcodec_free_context(&codec_ctx_);
}

void FFmpegCpuEncodingPipeline::start(EncodedFrameCallback callback) {
  if (running_.exchange(true)) return;
  callback_ = std::move(callback);
  worker_ = std::jthread([this]{ encode_loop(); });
}

void FFmpegCpuEncodingPipeline::stop() {
  if (!running_.exchange(false)) return;
  if (worker_.joinable()) worker_.join();
}

void FFmpegCpuEncodingPipeline::encode_loop() {
  using namespace std::chrono;
  const auto frame_duration = milliseconds(1000 / std::max(1, fps_));
  while (running_) {
    // Sentez bir desen üret (color bars)
    const int y_size = width_ * height_;
    const int uv_width = width_ / 2;
    const int uv_height = height_ / 2;
    const int u_size = uv_width * uv_height;
    const int v_size = u_size;

    av_frame_make_writable(frame_);
    std::memset(frame_->data[0], static_cast<unsigned char>((frame_id_ * 7) % 256), y_size);
    std::memset(frame_->data[1], static_cast<unsigned char>((frame_id_ * 3) % 256), u_size);
    std::memset(frame_->data[2], static_cast<unsigned char>((frame_id_ * 5) % 256), v_size);
    frame_->pts = static_cast<int64_t>(frame_id_);

    if (avcodec_send_frame(codec_ctx_, frame_) < 0) {
      continue;
    }
    while (avcodec_receive_packet(codec_ctx_, pkt_) == 0) {
      EncodedFrame out{};
      out.frame_id = frame_id_;
      out.timestamp_ns = static_cast<std::uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
      out.codec_fourcc = make_fourcc('H','2','6','4');
      out.is_keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
      out.data.resize(static_cast<std::size_t>(pkt_->size));
      std::memcpy(out.data.data(), pkt_->data, out.data.size());
      if (callback_) callback_(out);
      av_packet_unref(pkt_);
    }

    ++frame_id_;
    std::this_thread::sleep_for(frame_duration);
  }
}

} // namespace hydra::media


