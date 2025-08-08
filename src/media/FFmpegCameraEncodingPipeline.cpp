#include "hydra/media/FFmpegCameraEncodingPipeline.h"

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace hydra::media {

FFmpegCameraEncodingPipeline::FFmpegCameraEncodingPipeline(std::string device, int width, int height, int fps)
    : device_(std::move(device)), width_(width), height_(height), fps_(fps) {
  avdevice_register_all();
}

FFmpegCameraEncodingPipeline::~FFmpegCameraEncodingPipeline() {
  stop();
  if (sws_) sws_freeContext(sws_);
  if (pkt_) av_packet_free(&pkt_);
  if (frame_) av_frame_free(&frame_);
  if (enc_ctx_) avcodec_free_context(&enc_ctx_);
  if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
}

void FFmpegCameraEncodingPipeline::start(EncodedFrameCallback callback) {
  if (running_.exchange(true)) return;
  callback_ = std::move(callback);
  worker_ = std::jthread([this]{ loop(); });
}

void FFmpegCameraEncodingPipeline::stop() {
  if (!running_.exchange(false)) return;
  if (worker_.joinable()) worker_.join();
}

void FFmpegCameraEncodingPipeline::loop() {
  // Open camera via avdevice (v4l2)
  AVInputFormat* ifmt = av_find_input_format("video4linux2");
  AVDictionary* options = nullptr;
  av_dict_set(&options, "video_size", (std::to_string(width_) + "x" + std::to_string(height_)).c_str(), 0);
  av_dict_set(&options, "framerate", std::to_string(fps_).c_str(), 0);
  if (avformat_open_input(&fmt_ctx_, device_.c_str(), ifmt, &options) < 0) {
    running_ = false;
    return;
  }
  if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
    running_ = false; return;
  }
  for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
    if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index_ = static_cast<int>(i);
      break;
    }
  }
  if (video_stream_index_ < 0) { running_ = false; return; }

  // Create H.264 encoder
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  enc_ctx_ = avcodec_alloc_context3(codec);
  enc_ctx_->width = width_;
  enc_ctx_->height = height_;
  enc_ctx_->time_base = AVRational{1, fps_};
  enc_ctx_->framerate = AVRational{fps_, 1};
  enc_ctx_->gop_size = fps_ * 2;
  enc_ctx_->max_b_frames = 0;
  enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
  av_opt_set(enc_ctx_->priv_data, "preset", "veryfast", 0);
  if (avcodec_open2(enc_ctx_, codec, nullptr) < 0) { running_ = false; return; }

  frame_ = av_frame_alloc();
  frame_->format = enc_ctx_->pix_fmt;
  frame_->width = enc_ctx_->width;
  frame_->height = enc_ctx_->height;
  av_frame_get_buffer(frame_, 32);
  pkt_ = av_packet_alloc();

  // Prepare scaler from camera native to YUV420P
  AVCodecParameters* cpar = fmt_ctx_->streams[video_stream_index_]->codecpar;
  SwsContext* sws = sws_getContext(cpar->width, cpar->height, static_cast<AVPixelFormat>(cpar->format),
                                   width_, height_, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
  sws_ = sws;
  AVFrame* cam_frame = av_frame_alloc();

  AVPacket* in_pkt = av_packet_alloc();
  while (running_) {
    if (av_read_frame(fmt_ctx_, in_pkt) < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    if (in_pkt->stream_index != video_stream_index_) { av_packet_unref(in_pkt); continue; }

    // Decode raw frame from camera when needed
    // Many v4l2 devices can output raw; to keep simple, use libavcodec's decoder when required
    const AVCodec* dec = avcodec_find_decoder(cpar->codec_id);
    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, cpar);
    avcodec_open2(dec_ctx, dec, nullptr);
    avcodec_send_packet(dec_ctx, in_pkt);
    while (avcodec_receive_frame(dec_ctx, cam_frame) == 0) {
      av_frame_make_writable(frame_);
      sws_scale(sws_, cam_frame->data, cam_frame->linesize, 0, cam_frame->height, frame_->data, frame_->linesize);
      frame_->pts = static_cast<int64_t>(frame_id_);
      if (avcodec_send_frame(enc_ctx_, frame_) == 0) {
        while (avcodec_receive_packet(enc_ctx_, pkt_) == 0) {
          EncodedFrame out{};
          out.frame_id = frame_id_;
          out.timestamp_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
          out.codec_fourcc = make_fourcc('H','2','6','4');
          out.is_keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
          out.data.resize(static_cast<std::size_t>(pkt_->size));
          std::memcpy(out.data.data(), pkt_->data, out.data.size());
          if (callback_) callback_(out);
          av_packet_unref(pkt_);
        }
      }
      ++frame_id_;
      av_frame_unref(cam_frame);
    }
    avcodec_free_context(&dec_ctx);
    av_packet_unref(in_pkt);
  }

  av_packet_free(&in_pkt);
  av_frame_free(&cam_frame);
}

} // namespace hydra::media


