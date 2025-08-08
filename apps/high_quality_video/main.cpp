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

class HighQualityCamera {
private:
    int fd_ = -1;
    std::vector<VideoBuffer> buffers_;
    int width_ = 640;   // 720p yerine 640x480 (hızlı ama kaliteli)
    int height_ = 480;
    AVCodecContext* encoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;

public:
    bool init(const std::string& device) {
        // V4L2 aygıtını aç
        fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return false;

        // VGA çözünürlük ayarla (640x480)
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) return false;

        // 60 FPS ayarla
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

        // HIGH QUALITY H.264 encoder
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        encoder_ = avcodec_alloc_context3(codec);
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->time_base = {1, 60};
        encoder_->framerate = {60, 1};
        encoder_->gop_size = 30;  // Normal GOP
        encoder_->max_b_frames = 2;  // B frames için daha iyi compression
        encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
        
        // HIGH QUALITY ayarlar
        av_opt_set(encoder_->priv_data, "preset", "medium", 0);  // Daha kaliteli
        av_opt_set(encoder_->priv_data, "tune", "film", 0);      // Film kalitesi
        av_opt_set(encoder_->priv_data, "profile", "high", 0);   // High profile
        av_opt_set_int(encoder_->priv_data, "crf", 18, 0);      // Yüksek kalite (18 = excellent)
        av_opt_set_int(encoder_->priv_data, "threads", 4, 0);   // Multi-thread
        
        // Bitrate ayarları (yüksek kalite için)
        encoder_->bit_rate = 2000000;  // 2 Mbps
        encoder_->rc_max_rate = 3000000;  // 3 Mbps max
        encoder_->rc_buffer_size = 4000000;  // 4 Mbps buffer
        
        if (avcodec_open2(encoder_, codec, nullptr) < 0) return false;

        frame_ = av_frame_alloc();
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width = width_;
        frame_->height = height_;
        av_frame_get_buffer(frame_, 32);

        packet_ = av_packet_alloc();

        // YUV422 → YUV420P high quality scaler
        sws_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                             width_, height_, AV_PIX_FMT_YUV420P,
                             SWS_BICUBIC, nullptr, nullptr, nullptr);  // En kaliteli algoritma

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

        // YUYV → YUV420P dönüşümü (high quality)
        const uint8_t* yuyv_data = static_cast<uint8_t*>(buffers_[buf.index].start);
        uint8_t* src_data[1] = {const_cast<uint8_t*>(yuyv_data)};
        int src_linesize[1] = {width_ * 2};
        
        sws_scale(sws_, src_data, src_linesize, 0, height_,
                 frame_->data, frame_->linesize);

        frame_->pts++;

        // High quality encode
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

    ~HighQualityCamera() {
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

class HighQualityDecoder {
private:
    AVCodecContext* decoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;

public:
    bool init() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        decoder_ = avcodec_alloc_context3(codec);
        
        // HIGH QUALITY decode ayarları
        decoder_->flags2 |= AV_CODEC_FLAG2_FAST;  // Hız için
        // Skip filter'ları kaldırdık - kalite için
        av_opt_set_int(decoder_->priv_data, "threads", 4, 0);  // Multi-thread
        
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

    ~HighQualityDecoder() {
        if (packet_) av_packet_free(&packet_);
        if (frame_) av_frame_free(&frame_);
        if (decoder_) avcodec_free_context(&decoder_);
    }
};

class HDRenderer {
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
        
        // HD window (720p display)
        window_ = SDL_CreateWindow(title.c_str(), 
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) return false;

        renderer_ = SDL_CreateRenderer(window_, -1, 
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) {
            // Fallback to software renderer
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) return false;

        // High quality scaling
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");  // Best quality

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

    ~HDRenderer() {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Kullanim: high_quality_video <peer_ip> <local_port> [peer_port] [device]" << std::endl;
        std::cout << "Ornek: high_quality_video 192.168.1.5 8000 8001 /dev/video0" << std::endl;
        return 1;
    }

    std::string peer_ip = argv[1];
    uint16_t local_port = static_cast<uint16_t>(std::stoi(argv[2]));
    uint16_t peer_port = (argc >= 4) ? static_cast<uint16_t>(std::stoi(argv[3])) : local_port;
    std::string device = (argc >= 5) ? argv[4] : "/dev/video0";

    // High quality components
    HighQualityCamera camera;
    HighQualityDecoder decoder1, decoder2;  // Ayrı decoder'lar
    HDRenderer self_renderer, peer_renderer;
    
    if (!camera.init(device)) {
        std::cerr << "Kamera baslatma hatasi!" << std::endl;
        return 1;
    }

    if (!decoder1.init() || !decoder2.init()) {
        std::cerr << "Decoder baslatma hatasi!" << std::endl;
        return 1;
    }

    if (!self_renderer.init(640, 480, "BEN - HD VIDEO") || 
        !peer_renderer.init(640, 480, "ARKADASIM - HD VIDEO: " + peer_ip)) {
        std::cerr << "Renderer baslatma hatasi!" << std::endl;
        return 1;
    }

    // Network (3 port for high bandwidth)
    hydra::network::NetworkSender sender(peer_ip, {peer_port, static_cast<uint16_t>(peer_port + 1), static_cast<uint16_t>(peer_port + 2)});
    hydra::network::NetworkReceiver receiver({local_port, static_cast<uint16_t>(local_port + 1), static_cast<uint16_t>(local_port + 2)});
    hydra::network::Packetizer packetizer;
    hydra::network::Depacketizer depacketizer;

    // Receive işlemi (async)
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

    std::cout << "=== HIGH QUALITY VIDEO ENGINE ===" << std::endl;
    std::cout << "Peer: " << peer_ip << ":" << peer_port << "-" << (peer_port + 2) << std::endl;
    std::cout << "Local: " << local_port << "-" << (local_port + 2) << std::endl;
    std::cout << "Resolution: 640x480@60fps (VGA)" << std::endl;
    std::cout << "Codec: H.264 High Profile, CRF=18" << std::endl;
    std::cout << "Bitrate: 2 Mbps (High Quality)" << std::endl;
    std::cout << "Display: 1280x720 HD Windows" << std::endl;

    uint64_t seq = 0;
    auto last_frame_time = std::chrono::high_resolution_clock::now();
    
    while (true) {
        auto current_time = std::chrono::high_resolution_clock::now();
        auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            current_time - last_frame_time);
        
        // 60 FPS = 16.67ms per frame
        if (frame_duration.count() >= 16670) {
            last_frame_time = current_time;
            
            // Kamera capture ve encode
            camera.capture_and_encode([&](const std::vector<uint8_t>& encoded_data, bool is_keyframe) {
                // Network'e gönder (3 port üzerinden)
                hydra::media::EncodedFrame frame;
                frame.frame_id = seq++;
                frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    current_time.time_since_epoch()).count();
                frame.codec_fourcc = 0x34363248;  // H264
                frame.is_keyframe = is_keyframe;
                frame.data.resize(encoded_data.size());
                std::memcpy(frame.data.data(), encoded_data.data(), encoded_data.size());

                auto packets = packetizer.packetize(frame, seq);
                for (const auto& p : packets) sender.send(p);

                // Kendi görüntümüzü göster (high quality)
                decoder2.decode(encoded_data, [&](uint8_t* y_plane, int w, int h, int* linesize) {
                    self_renderer.render(y_plane, w, h, linesize);
                });
            });
        }

        // Event handling
        self_renderer.poll();
        peer_renderer.poll();

        // 60 FPS timing
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
