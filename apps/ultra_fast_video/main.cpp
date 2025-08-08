#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
}

#include <SDL2/SDL.h>
#include "../../include/hydra/network/NetworkSender.h"
#include "../../include/hydra/network/NetworkReceiver.h"
#include "../../include/hydra/network/Packetizer.h"

using namespace std::chrono_literals;

struct VideoBuffer {
    void* start;
    size_t length;
};

// ULTRA FAST CODEC - ZERO DELAY OPTIMIZED
class UltraFastCodec {
private:
    AVCodecContext* encoder_;
    AVCodecContext* decoder_;
    AVFrame* input_frame_;
    AVFrame* output_frame_;
    AVPacket* packet_;
    SwsContext* sws_ctx_;
    SwsContext* flip_ctx_;  // Kamera aynalama için
    int width_, height_;
    bool initialized_;
    
public:
    UltraFastCodec(int width = 320, int height = 240) 
        : encoder_(nullptr), decoder_(nullptr), 
          input_frame_(nullptr), output_frame_(nullptr), packet_(nullptr),
          sws_ctx_(nullptr), flip_ctx_(nullptr), width_(width), height_(height), initialized_(false) {}
    
    ~UltraFastCodec() {
        cleanup();
    }
    
    bool init() {
        // 1. ULTRA FAST ENCODER - MINIMUM DELAY
        const AVCodec* enc_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!enc_codec) {
            std::cerr << "H.264 encoder not found" << std::endl;
            return false;
        }
        
        encoder_ = avcodec_alloc_context3(enc_codec);
        if (!encoder_) {
            std::cerr << "Failed to allocate encoder" << std::endl;
            return false;
        }
        
        // ULTRA FAST SETTINGS - MINIMUM DELAY
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->time_base = {1, 60};  // 60 FPS
        encoder_->framerate = {60, 1};
        encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder_->bit_rate = 500000;  // 500 kbps - çok düşük
        encoder_->gop_size = 5;  // Her 5 frame'de keyframe - çok sık
        encoder_->max_b_frames = 0;  // B frame yok
        
        // ULTRA FAST PRESET - MINIMUM ENCODING TIME
        av_opt_set(encoder_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder_->priv_data, "tune", "zerolatency", 0);
        av_opt_set(encoder_->priv_data, "profile", "baseline", 0);
        av_opt_set_int(encoder_->priv_data, "crf", 35, 0);  // Düşük kalite, yüksek hız
        av_opt_set_int(encoder_->priv_data, "threads", 1, 0);  // Tek thread - daha hızlı
        av_opt_set_int(encoder_->priv_data, "slices", 1, 0);  // Tek slice
        av_opt_set_int(encoder_->priv_data, "sync-lookahead", 0, 0);  // Lookahead kapalı
        
        if (avcodec_open2(encoder_, enc_codec, nullptr) < 0) {
            std::cerr << "Failed to open encoder" << std::endl;
            return false;
        }
        
        // 2. ULTRA FAST DECODER
        const AVCodec* dec_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!dec_codec) {
            std::cerr << "H.264 decoder not found" << std::endl;
            return false;
        }
        
        decoder_ = avcodec_alloc_context3(dec_codec);
        if (!decoder_) {
            std::cerr << "Failed to allocate decoder" << std::endl;
            return false;
        }
        
        // ULTRA FAST DECODER
        decoder_->thread_count = 1;  // Tek thread
        decoder_->flags2 |= AV_CODEC_FLAG2_FAST;  // Hızlı decode
        decoder_->flags |= AV_CODEC_FLAG_LOW_DELAY;  // Düşük gecikme
        decoder_->err_recognition = AV_EF_IGNORE_ERR;  // Hataları görmezden gel
        
        if (avcodec_open2(decoder_, dec_codec, nullptr) < 0) {
            std::cerr << "Failed to open decoder" << std::endl;
            return false;
        }
        
        // 3. ALLOCATE FRAMES
        input_frame_ = av_frame_alloc();
        output_frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        
        if (!input_frame_ || !output_frame_ || !packet_) {
            std::cerr << "Failed to allocate frames/packets" << std::endl;
            return false;
        }
        
        // Setup input frame
        input_frame_->format = AV_PIX_FMT_YUV420P;
        input_frame_->width = width_;
        input_frame_->height = height_;
        if (av_frame_get_buffer(input_frame_, 32) < 0) {
            std::cerr << "Failed to allocate input frame buffer" << std::endl;
            return false;
        }
        
        // Setup SWS context - YUYV to YUV420P
        sws_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                                 width_, height_, AV_PIX_FMT_YUV420P,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);  // En hızlı interpolation
        
        // Setup FLIP context - KAMERA AYNALAMA
        flip_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                                  width_, height_, AV_PIX_FMT_YUYV422,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        
        if (!sws_ctx_ || !flip_ctx_) {
            std::cerr << "Failed to create SWS context" << std::endl;
            return false;
        }
        
        initialized_ = true;
        std::cout << "Ultra fast codec initialized: " << width_ << "x" << height_ << "@60fps with camera mirror" << std::endl;
        return true;
    }
    
    std::vector<uint8_t> encode_frame(uint8_t* yuyv_data) {
        if (!initialized_) return {};
        
        // KAMERA AYNALAMA - Horizontal flip
        std::vector<uint8_t> flipped_data(width_ * height_ * 2);  // YUYV = 2 bytes per pixel
        
        // Horizontal flip için satır satır kopyala
        for (int y = 0; y < height_; y++) {
            for (int x = 0; x < width_; x++) {
                int src_offset = (y * width_ + x) * 2;
                int dst_offset = (y * width_ + (width_ - 1 - x)) * 2;  // X koordinatını ters çevir
                
                // YUYV pixel'ini kopyala
                flipped_data[dst_offset] = yuyv_data[src_offset];        // Y
                flipped_data[dst_offset + 1] = yuyv_data[src_offset + 1]; // U/V
            }
        }
        
        // Convert flipped YUYV to YUV420P
        const uint8_t* src_data[4] = { flipped_data.data(), nullptr, nullptr, nullptr };
        int src_linesize[4] = { width_ * 2, 0, 0, 0 };
        
        sws_scale(sws_ctx_, src_data, src_linesize, 0, height_,
                 input_frame_->data, input_frame_->linesize);
        
        // Set PTS
        static int64_t pts = 0;
        input_frame_->pts = pts++;
        
        // ULTRA FAST ENCODE
        int ret = avcodec_send_frame(encoder_, input_frame_);
        if (ret < 0) return {};
        
        ret = avcodec_receive_packet(encoder_, packet_);
        if (ret == AVERROR(EAGAIN)) return {};
        if (ret < 0) return {};
        
        // Copy data
        std::vector<uint8_t> encoded_data(packet_->size);
        std::memcpy(encoded_data.data(), packet_->data, packet_->size);
        
        av_packet_unref(packet_);
        return encoded_data;
    }
    
    bool decode_frame(const std::vector<uint8_t>& encoded_data, 
                     std::function<void(AVFrame*)> callback) {
        if (!initialized_ || encoded_data.empty()) return false;
        
        packet_->data = const_cast<uint8_t*>(encoded_data.data());
        packet_->size = encoded_data.size();
        
        int ret = avcodec_send_packet(decoder_, packet_);
        if (ret < 0) return false;
        
        ret = avcodec_receive_frame(decoder_, output_frame_);
        if (ret == AVERROR(EAGAIN)) return true;
        if (ret < 0) return false;
        
        callback(output_frame_);
        return true;
    }
    
    void cleanup() {
        if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
        if (flip_ctx_) { sws_freeContext(flip_ctx_); flip_ctx_ = nullptr; }
        if (input_frame_) { av_frame_free(&input_frame_); }
        if (output_frame_) { av_frame_free(&output_frame_); }
        if (packet_) { av_packet_free(&packet_); }
        if (encoder_) { avcodec_free_context(&encoder_); }
        if (decoder_) { avcodec_free_context(&decoder_); }
    }
};

// ULTRA FAST CAMERA - MINIMUM CAPTURE DELAY
class UltraFastCamera {
private:
    int fd_;
    std::vector<VideoBuffer> buffers_;
    int width_, height_;
    bool initialized_;
    
public:
    UltraFastCamera() : fd_(-1), width_(320), height_(240), initialized_(false) {}
    
    ~UltraFastCamera() {
        cleanup();
    }
    
    bool init(const std::string& device = "/dev/video0") {
        fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ == -1) {
            std::cerr << "Failed to open camera device: " << device << std::endl;
            return false;
        }
        
        // Set format - DÜŞÜK ÇÖZÜNÜRLÜK, YÜKSEK FPS
        struct v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;  // Progressive
        
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) == -1) {
            std::cerr << "Failed to set camera format" << std::endl;
            return false;
        }
        
        // Set HIGH FPS - 60 FPS
        struct v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = 60;  // 60 FPS
        ioctl(fd_, VIDIOC_S_PARM, &parm);
        
        // MINIMUM BUFFERS - Az buffer, düşük gecikme
        struct v4l2_requestbuffers req{};
        req.count = 2;  // Sadece 2 buffer
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) == -1) {
            std::cerr << "Failed to request buffers" << std::endl;
            return false;
        }
        
        buffers_.resize(req.count);
        for (unsigned int i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) == -1) {
                std::cerr << "Failed to query buffer " << i << std::endl;
                return false;
            }
            
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd_, buf.m.offset);
            
            if (buffers_[i].start == MAP_FAILED) {
                std::cerr << "Failed to mmap buffer " << i << std::endl;
                return false;
            }
            
            if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
                std::cerr << "Failed to queue buffer " << i << std::endl;
                return false;
            }
        }
        
        // Start streaming
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
            std::cerr << "Failed to start streaming" << std::endl;
            return false;
        }
        
        initialized_ = true;
        std::cout << "Ultra fast camera initialized: " << width_ << "x" << height_ << "@60fps (mirrored)" << std::endl;
        return true;
    }
    
    uint8_t* capture_frame() {
        if (!initialized_) return nullptr;
        
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) return nullptr;
            return nullptr;
        }
        
        uint8_t* frame_data = static_cast<uint8_t*>(buffers_[buf.index].start);
        
        // IMMEDIATELY requeue - minimum delay
        if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
            std::cerr << "Failed to requeue buffer" << std::endl;
        }
        
        return frame_data;
    }
    
    void cleanup() {
        if (initialized_) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        
        for (auto& buffer : buffers_) {
            if (buffer.start != MAP_FAILED) {
                munmap(buffer.start, buffer.length);
            }
        }
        
        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }
    }
};

// ULTRA FAST RENDERER - MINIMUM DISPLAY DELAY
class UltraFastRenderer {
private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    SDL_Texture* texture_;
    int width_, height_;
    bool initialized_;
    
public:
    UltraFastRenderer(const std::string& title, int width = 320, int height = 240)
        : window_(nullptr), renderer_(nullptr), texture_(nullptr),
          width_(width), height_(height), initialized_(false) {
        
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
            return;
        }
        
        // Create window - SMALLER SIZE FOR SPEED
        window_ = SDL_CreateWindow(title.c_str(),
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 width * 2, height * 2,  // 2x büyütme
                                 SDL_WINDOW_SHOWN);
        if (!window_) {
            std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
            return;
        }
        
        // HARDWARE ACCELERATED RENDERER
        renderer_ = SDL_CreateRenderer(window_, -1, 
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) {
            // Fallback to software
            renderer_ = SDL_CreateRenderer(window_, -1, 0);
        }
        if (!renderer_) {
            std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
            return;
        }
        
        // FAST TEXTURE - YV12 format
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_YV12,
                                   SDL_TEXTUREACCESS_STREAMING, width_, height_);
        if (!texture_) {
            std::cerr << "Failed to create texture: " << SDL_GetError() << std::endl;
            return;
        }
        
        initialized_ = true;
        std::cout << "Ultra fast renderer initialized: " << title << " " << width << "x" << height << std::endl;
    }
    
    ~UltraFastRenderer() {
        cleanup();
    }
    
    void render(AVFrame* frame) {
        if (!initialized_ || !frame) return;
        
        // ULTRA FAST RENDER
        SDL_UpdateYUVTexture(texture_, nullptr,
                            frame->data[0], frame->linesize[0],
                            frame->data[1], frame->linesize[1],
                            frame->data[2], frame->linesize[2]);
        
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }
    
    bool poll() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                return false;
            }
        }
        return true;
    }
    
    void cleanup() {
        if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
        if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
        if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
        if (initialized_) { SDL_Quit(); initialized_ = false; }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <peer_ip> <local_port> [peer_port]" << std::endl;
        std::cout << "Example: " << argv[0] << " 192.168.1.5 5000 5001" << std::endl;
        return -1;
    }
    
    std::string peer_ip = argv[1];
    int local_port = std::atoi(argv[2]);
    int peer_port = (argc > 3) ? std::atoi(argv[3]) : (local_port + 1);
    
    std::cout << "=== ULTRA FAST VIDEO ENGINE - ZERO DELAY + MIRRORED CAMERA ===" << std::endl;
    std::cout << "Peer IP: " << peer_ip << std::endl;
    std::cout << "Local Port: " << local_port << std::endl;
    std::cout << "Peer Port: " << peer_port << std::endl;
    std::cout << "Resolution: 320x240@60fps (optimized for speed)" << std::endl;
    std::cout << "Camera: MIRRORED (self-view looks natural)" << std::endl;
    
    // Initialize components
    UltraFastCamera camera;
    UltraFastCodec self_codec(320, 240);
    UltraFastCodec peer_codec(320, 240);
    UltraFastRenderer self_renderer("SELF VIEW (MIRRORED)", 320, 240);
    UltraFastRenderer peer_renderer("PEER VIEW", 320, 240);
    
    // Initialize all components
    if (!camera.init()) {
        std::cerr << "Failed to initialize camera" << std::endl;
        return -1;
    }
    
    if (!self_codec.init() || !peer_codec.init()) {
        std::cerr << "Failed to initialize codecs" << std::endl;
        return -1;
    }
    
    // Network setup
    hydra::network::NetworkSender sender(peer_ip, {static_cast<uint16_t>(peer_port)});
    hydra::network::NetworkReceiver receiver({static_cast<uint16_t>(local_port)});
    hydra::network::Packetizer packetizer;
    hydra::network::Depacketizer depacketizer;
    
    uint64_t seq_base = 0;
    
    // Network receiver callback
    receiver.start([&](const asio::ip::udp::endpoint& endpoint, const hydra::network::Packet& packet) {
        auto frame_opt = depacketizer.push_and_try_reassemble(packet);
        if (frame_opt) {
            // Convert std::byte to uint8_t
            std::vector<uint8_t> data;
            data.reserve(frame_opt->data.size());
            for (auto byte : frame_opt->data) {
                data.push_back(static_cast<uint8_t>(byte));
            }
            
            peer_codec.decode_frame(data, [&](AVFrame* frame) {
                peer_renderer.render(frame);
            });
        }
    });
    
    std::cout << "Ultra fast video engine started. ESC to quit." << std::endl;
    std::cout << "OPTIMIZATIONS:" << std::endl;
    std::cout << "- 320x240 resolution (low latency)" << std::endl;
    std::cout << "- 60 FPS capture/encode" << std::endl;
    std::cout << "- 2 camera buffers only" << std::endl;
    std::cout << "- Keyframe every 5 frames" << std::endl;
    std::cout << "- Single-threaded encoding" << std::endl;
    std::cout << "- Hardware accelerated rendering" << std::endl;
    std::cout << "- Camera horizontally mirrored" << std::endl;
    
    // ULTRA FAST MAIN LOOP
    auto last_time = std::chrono::steady_clock::now();
    
    while (true) {
        auto current_time = std::chrono::steady_clock::now();
        
        // Capture and encode
        uint8_t* yuyv_data = camera.capture_frame();
        if (yuyv_data) {
            auto encoded_data = self_codec.encode_frame(yuyv_data);
            if (!encoded_data.empty()) {
                // Create frame for network
                hydra::media::EncodedFrame frame;
                frame.frame_id = seq_base;
                frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    current_time.time_since_epoch()).count();
                frame.codec_fourcc = 0x48323634; // "H264"
                
                // Convert uint8_t to std::byte
                frame.data.reserve(encoded_data.size());
                for (auto byte : encoded_data) {
                    frame.data.push_back(static_cast<std::byte>(byte));
                }
                
                // Send over network IMMEDIATELY
                auto packets = packetizer.packetize(frame, seq_base);
                seq_base += packets.size();
                for (const auto& p : packets) {
                    sender.send(p);
                }
                
                // Decode and show self view IMMEDIATELY
                self_codec.decode_frame(encoded_data, [&](AVFrame* frame) {
                    self_renderer.render(frame);
                });
            }
        }
        
        // Poll events
        if (!self_renderer.poll() || !peer_renderer.poll()) {
            break;
        }
        
        // MINIMAL SLEEP - 60 FPS timing (16.67ms)
        auto elapsed = std::chrono::steady_clock::now() - current_time;
        auto target_time = 16ms;
        if (elapsed < target_time) {
            std::this_thread::sleep_for(target_time - elapsed);
        }
    }
    
    std::cout << "Ultra fast video engine stopped." << std::endl;
    return 0;
}
