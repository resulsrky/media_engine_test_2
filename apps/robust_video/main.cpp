#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <queue>

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

// Robust Camera with Error Resilience
class RobustCamera {
private:
    int fd_;
    std::vector<VideoBuffer> buffers_;
    AVCodecContext* encoder_;
    SwsContext* sws_ctx_;
    AVFrame* frame_;
    AVFrame* yuv_frame_;
    AVPacket* packet_;
    int width_, height_;
    bool initialized_;

public:
    RobustCamera() : fd_(-1), encoder_(nullptr), sws_ctx_(nullptr), 
                     frame_(nullptr), yuv_frame_(nullptr), packet_(nullptr),
                     width_(640), height_(480), initialized_(false) {}

    ~RobustCamera() {
        cleanup();
    }

    bool init(const std::string& device = "/dev/video0") {
        // Open V4L2 device
        fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ == -1) {
            std::cerr << "Cannot open camera device: " << device << std::endl;
            return false;
        }

        // Set format
        struct v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) == -1) {
            std::cerr << "Failed to set format" << std::endl;
            return false;
        }

        // Set frame rate - more conservative for stability
        struct v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = 25; // 25 FPS - daha kararlı
        ioctl(fd_, VIDIOC_S_PARM, &parm);

        // Setup memory mapping
        struct v4l2_requestbuffers req{};
        req.count = 6; // Daha fazla buffer - paket kaybına karşı
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

            // Queue buffer
            if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
                std::cerr << "Failed to queue buffer " << i << std::endl;
                return false;
            }
        }

        // Initialize FFmpeg encoder with robust settings
        if (!init_encoder()) {
            return false;
        }

        // Start streaming
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
            std::cerr << "Failed to start streaming" << std::endl;
            return false;
        }

        initialized_ = true;
        std::cout << "Robust camera initialized: " << width_ << "x" << height_ << "@30fps" << std::endl;
        return true;
    }

private:
    bool init_encoder() {
        // Find H.264 encoder
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cerr << "H.264 encoder not found" << std::endl;
            return false;
        }

        encoder_ = avcodec_alloc_context3(codec);
        if (!encoder_) {
            std::cerr << "Failed to allocate encoder context" << std::endl;
            return false;
        }

        // PPS SORUNUNU ÇÖZEN AYARLAR - Her keyframe'de SPS/PPS gönder
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->time_base = {1, 25}; // 25 FPS
        encoder_->framerate = {25, 1};
        encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder_->bit_rate = 800000; // 800 kbps - daha düşük, daha kararlı
        encoder_->rc_max_rate = 1200000; // 1.2 Mbps max
        encoder_->rc_buffer_size = 800000; // 800KB buffer
        encoder_->gop_size = 10; // ÇOK SIK KEYFRAME - her 10 frame'de (0.33 saniye)
        encoder_->max_b_frames = 0; // No B-frames
        encoder_->keyint_min = 10; // Min keyframe interval da 10
        
        // GLOBAL HEADER - SPS/PPS her zaman başta
        encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        
        // ULTRA ROBUST encoder options
        av_opt_set(encoder_->priv_data, "preset", "ultrafast", 0); // En hızlı
        av_opt_set(encoder_->priv_data, "tune", "zerolatency", 0); 
        av_opt_set(encoder_->priv_data, "profile", "baseline", 0); // Baseline profile
        av_opt_set_int(encoder_->priv_data, "crf", 30, 0); // Daha düşük kalite ama kararlı
        av_opt_set_int(encoder_->priv_data, "threads", 1, 0); // Tek thread - daha kararlı
        
        // PPS TEKRARLAMA ve hata direnci
        av_opt_set_int(encoder_->priv_data, "slice-max-size", 500, 0); // Daha küçük slice'lar
        av_opt_set_int(encoder_->priv_data, "intra-refresh", 1, 0); // Intra refresh
        av_opt_set_int(encoder_->priv_data, "forced-idr", 1, 0); // Force IDR frames
        av_opt_set_int(encoder_->priv_data, "repeat-headers", 1, 0); // SPS/PPS tekrarla
        av_opt_set_int(encoder_->priv_data, "aud", 1, 0); // Access unit delimiters
        av_opt_set_int(encoder_->priv_data, "sc_threshold", 0, 0); // Scene change kapalı
        
        if (avcodec_open2(encoder_, codec, nullptr) < 0) {
            std::cerr << "Failed to open encoder" << std::endl;
            return false;
        }

        // Allocate frames
        frame_ = av_frame_alloc();
        yuv_frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();

        if (!frame_ || !yuv_frame_ || !packet_) {
            std::cerr << "Failed to allocate frames/packet" << std::endl;
            return false;
        }

        // Setup YUV frame
        yuv_frame_->format = AV_PIX_FMT_YUV420P;
        yuv_frame_->width = width_;
        yuv_frame_->height = height_;
        if (av_frame_get_buffer(yuv_frame_, 32) < 0) {
            std::cerr << "Failed to allocate YUV frame buffer" << std::endl;
            return false;
        }

        // Setup RGB frame for conversion
        frame_->format = AV_PIX_FMT_YUYV422;
        frame_->width = width_;
        frame_->height = height_;

        // Initialize scaler
        sws_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                                width_, height_, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            std::cerr << "Failed to initialize scaler" << std::endl;
            return false;
        }

        return true;
    }

public:
    std::vector<uint8_t> capture_and_encode() {
        if (!initialized_) return {};

        // Dequeue buffer
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) return {}; // No frame ready
            std::cerr << "Failed to dequeue buffer" << std::endl;
            return {};
        }

        // Setup frame data
        frame_->data[0] = static_cast<uint8_t*>(buffers_[buf.index].start);
        frame_->linesize[0] = width_ * 2; // YUYV is 2 bytes per pixel

        // Convert YUYV to YUV420P
        sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, height_,
                 yuv_frame_->data, yuv_frame_->linesize);

        // Set PTS
        static int64_t pts = 0;
        yuv_frame_->pts = pts++;

        // Encode frame
        std::vector<uint8_t> encoded_data;
        int ret = avcodec_send_frame(encoder_, yuv_frame_);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder" << std::endl;
        } else {
            while (ret >= 0) {
                ret = avcodec_receive_packet(encoder_, packet_);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "Error encoding frame" << std::endl;
                    break;
                }

                // Copy encoded data
                encoded_data.resize(packet_->size);
                std::memcpy(encoded_data.data(), packet_->data, packet_->size);
                av_packet_unref(packet_);
                break; // Only take first packet
            }
        }

        // Re-queue buffer
        if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
            std::cerr << "Failed to re-queue buffer" << std::endl;
        }

        return encoded_data;
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
        }

        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
        }

        if (encoder_) {
            avcodec_free_context(&encoder_);
        }

        if (frame_) av_frame_free(&frame_);
        if (yuv_frame_) av_frame_free(&yuv_frame_);
        if (packet_) av_packet_free(&packet_);
    }
};

// Robust Decoder with Error Recovery
class RobustDecoder {
private:
    AVCodecContext* decoder_;
    AVFrame* frame_;
    AVPacket* packet_;
    bool initialized_;
    int error_count_;
    int max_errors_;

public:
    RobustDecoder() : decoder_(nullptr), frame_(nullptr), packet_(nullptr), 
                      initialized_(false), error_count_(0), max_errors_(10) {}

    ~RobustDecoder() {
        cleanup();
    }

    bool init() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cerr << "H.264 decoder not found" << std::endl;
            return false;
        }

        decoder_ = avcodec_alloc_context3(codec);
        if (!decoder_) {
            std::cerr << "Failed to allocate decoder context" << std::endl;
            return false;
        }

        // PPS HATALARINDAN KORUNMA - Ultra robust decoder settings
        decoder_->thread_count = 1; // Tek thread daha kararlı
        decoder_->thread_type = FF_THREAD_FRAME;
        
        // MÜKEMmEL ERROR CONCEALMENT - PPS kaybolsa bile devam et
        decoder_->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK | FF_EC_FAVOR_INTER;
        decoder_->err_recognition = AV_EF_IGNORE_ERR; // BÜTÜN HATALARI GÖRMEZDEN GEL!
        decoder_->skip_frame = AVDISCARD_NONE; // Hiç frame atma
        decoder_->skip_idct = AVDISCARD_NONE; // IDCT atma
        decoder_->skip_loop_filter = AVDISCARD_NONE; // Loop filter atma
        
        // PPS/SPS eksik olduğunda devam et
        decoder_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT; // Bozuk frame'leri bile göster
        decoder_->flags2 |= AV_CODEC_FLAG2_IGNORE_CROP; // Crop hatalarını görmezden gel
        decoder_->flags2 |= AV_CODEC_FLAG2_FAST; // Hızlı decode, hata kontrolü az

        if (avcodec_open2(decoder_, codec, nullptr) < 0) {
            std::cerr << "Failed to open decoder" << std::endl;
            return false;
        }

        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();

        if (!frame_ || !packet_) {
            std::cerr << "Failed to allocate decoder frames/packet" << std::endl;
            return false;
        }

        initialized_ = true;
        std::cout << "Robust decoder initialized with error recovery" << std::endl;
        return true;
    }

    bool decode(const std::vector<uint8_t>& data, std::function<void(AVFrame*)> callback) {
        if (!initialized_ || data.empty()) return false;

        // Reset error count on successful keyframes
        if (data.size() > 5 && data[4] == 0x67) { // SPS (keyframe indicator)
            error_count_ = 0;
        }

        packet_->data = const_cast<uint8_t*>(data.data());
        packet_->size = data.size();

        int ret = avcodec_send_packet(decoder_, packet_);
        if (ret < 0) {
            error_count_++;
            if (error_count_ < max_errors_) {
                std::cerr << "Warning: Failed to send packet to decoder (error " 
                         << error_count_ << "/" << max_errors_ << ")" << std::endl;
                return false; // Continue trying
            } else {
                std::cerr << "Too many decoder errors, reinitializing..." << std::endl;
                reinitialize();
                return false;
            }
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(decoder_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                error_count_++;
                std::cerr << "Warning: Error decoding frame" << std::endl;
                break;
            }

            error_count_ = 0; // Reset on success
            callback(frame_);
            break;
        }

        return true;
    }

private:
    void reinitialize() {
        cleanup();
        std::this_thread::sleep_for(100ms); // Brief pause
        init();
        error_count_ = 0;
    }

    void cleanup() {
        if (decoder_) {
            avcodec_free_context(&decoder_);
        }
        if (frame_) av_frame_free(&frame_);
        if (packet_) av_packet_free(&packet_);
        initialized_ = false;
    }
};

// SDL Renderer with Error Handling
class RobustRenderer {
private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    SDL_Texture* texture_;
    int width_, height_;
    std::string title_;

public:
    RobustRenderer(const std::string& title, int width, int height) 
        : window_(nullptr), renderer_(nullptr), texture_(nullptr),
          width_(width), height_(height), title_(title) {}

    ~RobustRenderer() {
        cleanup();
    }

    bool init() {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
            return false;
        }

        window_ = SDL_CreateWindow(title_.c_str(),
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 width_, height_, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) {
            std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
            return false;
        }

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
            return false;
        }

        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_YV12,
                                   SDL_TEXTUREACCESS_STREAMING, width_, height_);
        if (!texture_) {
            std::cerr << "Failed to create texture: " << SDL_GetError() << std::endl;
            return false;
        }

        return true;
    }

    void render(AVFrame* frame) {
        if (!frame || !texture_) return;

        // Update texture with YUV data
        SDL_UpdateYUVTexture(texture_, nullptr,
                            frame->data[0], frame->linesize[0],
                            frame->data[1], frame->linesize[1],
                            frame->data[2], frame->linesize[2]);

        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    bool poll() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                return false;
            }
        }
        return true;
    }

private:
    void cleanup() {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Kullanım: " << argv[0] << " <peer_ip> <local_port> [peer_port]" << std::endl;
        std::cout << "Örnek: " << argv[0] << " 192.168.1.5 8000 8001" << std::endl;
        return 1;
    }

    std::string peer_ip = argv[1];
    int local_port = std::stoi(argv[2]);
    int peer_port = argc > 3 ? std::stoi(argv[3]) : local_port + 1;

    std::cout << "=== Robust Video Engine ===" << std::endl;
    std::cout << "Peer IP: " << peer_ip << std::endl;
    std::cout << "Local Port: " << local_port << std::endl;
    std::cout << "Peer Port: " << peer_port << std::endl;

    // Initialize FFmpeg
    av_log_set_level(AV_LOG_WARNING); // Reduce log spam

    // Initialize components
    RobustCamera camera;
    if (!camera.init()) {
        std::cerr << "Failed to initialize camera" << std::endl;
        return 1;
    }

    RobustDecoder self_decoder, peer_decoder;
    if (!self_decoder.init() || !peer_decoder.init()) {
        std::cerr << "Failed to initialize decoders" << std::endl;
        return 1;
    }

    RobustRenderer self_renderer("Kendi Görüntünüz - Robust", 640, 480);
    RobustRenderer peer_renderer("Arkadaşınızın Görüntüsü - Robust", 640, 480);
    
    if (!self_renderer.init() || !peer_renderer.init()) {
        std::cerr << "Failed to initialize renderers" << std::endl;
        return 1;
    }

    // Network components
    hydra::network::NetworkSender sender(peer_ip, {static_cast<uint16_t>(peer_port)});
    hydra::network::NetworkReceiver receiver({static_cast<uint16_t>(local_port)});
    hydra::network::Packetizer packetizer;
    hydra::network::Depacketizer depacketizer;

    // Setup network receiver callback
    receiver.start([&](const asio::ip::udp::endpoint& endpoint, const hydra::network::Packet& packet) {
        auto frame_opt = depacketizer.push_and_try_reassemble(packet);
        if (frame_opt) {
            // Convert std::byte to uint8_t
            std::vector<uint8_t> data;
            data.reserve(frame_opt->data.size());
            for (auto byte : frame_opt->data) {
                data.push_back(static_cast<uint8_t>(byte));
            }
            peer_decoder.decode(data, [&](AVFrame* frame) {
                peer_renderer.render(frame);
            });
        }
    });

    std::cout << "Robust video engine başlatıldı. ESC ile çıkış." << std::endl;

    // Main loop
    uint64_t frame_id = 0;
    auto last_frame_time = std::chrono::steady_clock::now();
    const auto frame_duration = 33ms; // ~30 FPS

    while (true) {
        auto current_time = std::chrono::steady_clock::now();
        
        // Capture and send
        if (current_time - last_frame_time >= frame_duration) {
            auto encoded_data = camera.capture_and_encode();
            if (!encoded_data.empty()) {
                // Create frame
                hydra::media::EncodedFrame frame;
                frame.frame_id = frame_id++;
                frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    current_time.time_since_epoch()).count();
                frame.codec_fourcc = 0x48323634; // "H264" as fourcc
                
                // Convert uint8_t to std::byte
                frame.data.reserve(encoded_data.size());
                for (auto byte : encoded_data) {
                    frame.data.push_back(static_cast<std::byte>(byte));
                }

                // Send to peer
                auto packets = packetizer.packetize(frame, frame_id * 100);
                for (const auto& p : packets) {
                    sender.send(p);
                }

                // Decode and show self - convert back to uint8_t for decoder
                std::vector<uint8_t> self_data;
                self_data.reserve(frame.data.size());
                for (auto byte : frame.data) {
                    self_data.push_back(static_cast<uint8_t>(byte));
                }
                self_decoder.decode(self_data, [&](AVFrame* decoded_frame) {
                    self_renderer.render(decoded_frame);
                });
            }
            last_frame_time = current_time;
        }

        // Poll events
        if (!self_renderer.poll() || !peer_renderer.poll()) {
            break;
        }

        // 25 FPS timing (40ms per frame)
        std::this_thread::sleep_for(40ms);
    }

    std::cout << "Robust video engine kapatılıyor..." << std::endl;
    return 0;
}
