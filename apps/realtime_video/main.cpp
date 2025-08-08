#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

#include <SDL2/SDL.h>

#include "hydra/network/NetworkSender.h"
#include "hydra/network/NetworkReceiver.h"
#include "hydra/network/Packetizer.h"

struct VideoBuffer {
    void* start;
    size_t length;
};

class UltraLowLatencyCamera {
private:
    int fd_ = -1;
    std::vector<VideoBuffer> buffers_;
    int width_ = 320;
    int height_ = 240;
    AVCodecContext* encoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    std::atomic<bool> running_{false};

public:
    bool init(const std::string& device) {
        // V4L2 aygıtını aç
        fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return false;

        // Düşük çözünürlük ayarla (320x240)
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) return false;

        // FPS ayarla (60 FPS)
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = 60;
        ioctl(fd_, VIDIOC_S_PARM, &parm);

        // Memory mapping
        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) return false;

        buffers_.resize(req.count);
        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) return false;

            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, 
                                   PROT_READ | PROT_WRITE, MAP_SHARED,
                                   fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) return false;
        }

        // Ultra-fast H.264 encoder
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        encoder_ = avcodec_alloc_context3(codec);
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->time_base = {1, 60};
        encoder_->framerate = {60, 1};
        encoder_->gop_size = 5;  // Çok küçük GOP
        encoder_->max_b_frames = 0;  // B frame yok
        encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
        
        // Ultra-fast ayarlar
        av_opt_set(encoder_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder_->priv_data, "tune", "zerolatency", 0);
        av_opt_set(encoder_->priv_data, "profile", "baseline", 0);
        av_opt_set_int(encoder_->priv_data, "crf", 28, 0);  // Kalite/hız dengesi
        
        if (avcodec_open2(encoder_, codec, nullptr) < 0) return false;

        frame_ = av_frame_alloc();
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width = width_;
        frame_->height = height_;
        av_frame_get_buffer(frame_, 32);

        packet_ = av_packet_alloc();

        // YUV422 → YUV420P scaler
        sws_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                             width_, height_, AV_PIX_FMT_YUV420P,
                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        return true;
    }

    bool start_stream() {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (unsigned i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
        }
        return ioctl(fd_, VIDIOC_STREAMON, &type) >= 0;
    }

    bool capture_and_encode(std::function<void(const std::vector<uint8_t>&, bool)> callback) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;

        // YUYV → YUV420P dönüşümü
        const uint8_t* yuyv_data = static_cast<uint8_t*>(buffers_[buf.index].start);
        uint8_t* src_data[1] = {const_cast<uint8_t*>(yuyv_data)};
        int src_linesize[1] = {width_ * 2};
        
        sws_scale(sws_, src_data, src_linesize, 0, height_,
                 frame_->data, frame_->linesize);

        frame_->pts++;

        // Encode
        if (avcodec_send_frame(encoder_, frame_) == 0) {
            while (avcodec_receive_packet(encoder_, packet_) == 0) {
                std::vector<uint8_t> encoded_data(packet_->data, packet_->data + packet_->size);
                bool is_keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
                callback(encoded_data, is_keyframe);
                av_packet_unref(packet_);
            }
        }

        // Buffer'ı geri koy
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
        return true;
    }

    ~UltraLowLatencyCamera() {
        if (sws_) sws_freeContext(sws_);
        if (packet_) av_packet_free(&packet_);
        if (frame_) av_frame_free(&frame_);
        if (encoder_) avcodec_free_context(&encoder_);
        
        for (auto& buf : buffers_) {
            if (buf.start) munmap(buf.start, buf.length);
        }
        if (fd_ >= 0) close(fd_);
    }
};

class UltraFastDecoder {
private:
    AVCodecContext* decoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;

public:
    bool init() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        decoder_ = avcodec_alloc_context3(codec);
        
        // Ultra-fast decode ayarları
        decoder_->flags2 |= AV_CODEC_FLAG2_FAST;
        decoder_->skip_loop_filter = AVDISCARD_ALL;
        decoder_->skip_idct = AVDISCARD_BIDIR;
        decoder_->skip_frame = AVDISCARD_BIDIR;
        
        if (avcodec_open2(decoder_, codec, nullptr) < 0) return false;

        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        return true;
    }

    bool decode(const std::vector<uint8_t>& data, 
                std::function<void(uint8_t*, int, int, int*)> callback) {
        packet_->data = const_cast<uint8_t*>(data.data());
        packet_->size = static_cast<int>(data.size());

        if (avcodec_send_packet(decoder_, packet_) == 0) {
            while (avcodec_receive_frame(decoder_, frame_) == 0) {
                callback(frame_->data[0], frame_->width, frame_->height, frame_->linesize);
                return true;
            }
        }
        return false;
    }

    ~UltraFastDecoder() {
        if (packet_) av_packet_free(&packet_);
        if (frame_) av_frame_free(&frame_);
        if (decoder_) avcodec_free_context(&decoder_);
    }
};

class SimpleRenderer {
private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int width_, height_;

public:
    bool init(int width, int height, const std::string& title) {
        width_ = width;
        height_ = height;
        
        if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
        
        window_ = SDL_CreateWindow(title.c_str(), 
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 width_, height_, SDL_WINDOW_SHOWN);
        if (!window_) return false;

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) return false;

        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_YV12,
                                   SDL_TEXTUREACCESS_STREAMING, width_, height_);
        return texture_ != nullptr;
    }

    void render(uint8_t* y_plane, int width, int height, int* linesize) {
        if (width != width_ || height != height_) return;
        
        SDL_UpdateYUVTexture(texture_, nullptr,
                            y_plane, linesize[0],
                            y_plane + linesize[0] * height, linesize[1],
                            y_plane + linesize[0] * height + linesize[1] * height / 4, linesize[2]);
        
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    void poll() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                exit(0);
            }
        }
    }

    ~SimpleRenderer() {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Kullanim: realtime_video <peer_ip> <local_port> [peer_port] [device]" << std::endl;
        std::cout << "Ornek: realtime_video 192.168.1.5 8000 8001 /dev/video0" << std::endl;
        return 1;
    }

    std::string peer_ip = argv[1];
    uint16_t local_port = static_cast<uint16_t>(std::stoi(argv[2]));
    uint16_t peer_port = (argc >= 4) ? static_cast<uint16_t>(std::stoi(argv[3])) : local_port;
    std::string device = (argc >= 5) ? argv[4] : "/dev/video0";

    // Ultra-düşük gecikme ayarları
    UltraLowLatencyCamera camera;
    UltraFastDecoder decoder1, decoder2;  // Ayrı decoder'lar
    SimpleRenderer self_renderer, peer_renderer;
    
    if (!camera.init(device)) {
        std::cerr << "Kamera baslatma hatasi!" << std::endl;
        return 1;
    }

    if (!decoder1.init() || !decoder2.init()) {
        std::cerr << "Decoder baslatma hatasi!" << std::endl;
        return 1;
    }

    if (!self_renderer.init(320, 240, "Ben") || 
        !peer_renderer.init(320, 240, "Peer: " + peer_ip)) {
        std::cerr << "Renderer baslatma hatasi!" << std::endl;
        return 1;
    }

    // Network
    hydra::network::NetworkSender sender(peer_ip, {peer_port});
    hydra::network::NetworkReceiver receiver({local_port});
    hydra::network::Packetizer packetizer;
    hydra::network::Depacketizer depacketizer;

    // Receive işlemi
    receiver.start([&](const asio::ip::udp::endpoint&, const hydra::network::Packet& pkt) {
        auto frame_opt = depacketizer.push_and_try_reassemble(pkt);
        if (frame_opt && !frame_opt->data.empty()) {
            std::vector<uint8_t> data(frame_opt->data.size());
            std::memcpy(data.data(), frame_opt->data.data(), frame_opt->data.size());
            decoder1.decode(data, [&](uint8_t* y_plane, int w, int h, int* linesize) {
                peer_renderer.render(y_plane, w, h, linesize);
            });
        }
    });

    camera.start_stream();

    std::cout << "=== ULTRA-LOW LATENCY VIDEO ENGINE ===" << std::endl;
    std::cout << "Peer: " << peer_ip << ":" << peer_port << std::endl;
    std::cout << "Local: " << local_port << std::endl;
    std::cout << "Resolution: 320x240@60fps" << std::endl;
    std::cout << "Ultra-fast H.264 encoding/decoding" << std::endl;

    uint64_t seq = 0;
    while (true) {
        // Kamera capture ve encode
        camera.capture_and_encode([&](const std::vector<uint8_t>& encoded_data, bool is_keyframe) {
            // Network'e gönder
            hydra::media::EncodedFrame frame;
            frame.frame_id = seq++;
            frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            frame.codec_fourcc = 0x34363248;  // H264
            frame.is_keyframe = is_keyframe;
            frame.data.resize(encoded_data.size());
            std::memcpy(frame.data.data(), encoded_data.data(), encoded_data.size());

            auto packets = packetizer.packetize(frame, seq);
            for (const auto& p : packets) sender.send(p);

            // Kendi görüntümüzü göster
            decoder2.decode(encoded_data, [&](uint8_t* y_plane, int w, int h, int* linesize) {
                self_renderer.render(y_plane, w, h, linesize);
            });
        });

        // Event handling
        self_renderer.poll();
        peer_renderer.poll();

        // Minimal sleep (16ms = ~60fps)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return 0;
}
