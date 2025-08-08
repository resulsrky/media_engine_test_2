#include "hydra/media/FFmpegDecoder.h"

#include <cstring>
#include <stdexcept>

namespace hydra::media {

FFmpegDecoder::FFmpegDecoder() {
  avcodec_register_all();
  frame_ = av_frame_alloc();
  yuv420_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
}

FFmpegDecoder::~FFmpegDecoder() {
  if (sws_) sws_freeContext(sws_);
  if (pkt_) av_packet_free(&pkt_);
  if (yuv420_) av_frame_free(&yuv420_);
  if (frame_) av_frame_free(&frame_);
  if (codec_ctx_) avcodec_free_context(&codec_ctx_);
}

void FFmpegDecoder::init_h264(int width_hint, int height_hint) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) throw std::runtime_error("H264 decoder not found");
  codec_ctx_ = avcodec_alloc_context3(codec);
  codec_ctx_->width = width_hint;
  codec_ctx_->height = height_hint;
  if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
    throw std::runtime_error("Failed to open H264 decoder");
  }
}

void FFmpegDecoder::push(const EncodedFrame& frame, DecodedCallback cb) {
  if (!codec_ctx_) init_h264(640, 360);
  av_packet_unref(pkt_);
  pkt_->data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(frame.data.data()));
  pkt_->size = static_cast<int>(frame.data.size());

  if (avcodec_send_packet(codec_ctx_, pkt_) < 0) return;
  while (avcodec_receive_frame(codec_ctx_, frame_) == 0) {
    // Convert to YUV420P if necessary
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame_->format);
    if (src_fmt != AV_PIX_FMT_YUV420P) {
      if (!sws_) {
        sws_ = sws_getContext(frame_->width, frame_->height, src_fmt,
                              frame_->width, frame_->height, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        yuv420_->format = AV_PIX_FMT_YUV420P;
        yuv420_->width = frame_->width;
        yuv420_->height = frame_->height;
        av_frame_get_buffer(yuv420_, 32);
      }
      sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height,
                yuv420_->data, yuv420_->linesize);
    }
    AVFrame* src = (src_fmt == AV_PIX_FMT_YUV420P) ? frame_ : yuv420_;

    DecodedFrame out{};
    out.frame_id = frame.frame_id;
    out.timestamp_ns = frame.timestamp_ns;
    out.width = src->width;
    out.height = src->height;
    out.format = PixelFormat::YUV420P;
    const int y_size = src->width * src->height;
    const int u_size = (src->width/2) * (src->height/2);
    const int v_size = u_size;
    out.plane_y.resize(y_size);
    out.plane_u.resize(u_size);
    out.plane_v.resize(v_size);
    for (int y = 0; y < src->height; ++y) {
      std::memcpy(out.plane_y.data() + y * src->width, src->data[0] + y * src->linesize[0], src->width);
    }
    for (int y = 0; y < src->height/2; ++y) {
      std::memcpy(out.plane_u.data() + y * (src->width/2), src->data[1] + y * src->linesize[1], src->width/2);
      std::memcpy(out.plane_v.data() + y * (src->width/2), src->data[2] + y * src->linesize[2], src->width/2);
    }
    if (cb) cb(out);
    av_frame_unref(frame_);
  }
}

} // namespace hydra::media


