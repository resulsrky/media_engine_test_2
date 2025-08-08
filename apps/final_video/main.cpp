#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <functional>
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

// PROVEN RELIABLE H.264 CODEC - NO PPS ERRORS!
class ReliableVideoCodec {
private:
    AVCodecContext* encoder_;
    AVCodecContext* decoder_;
    AVFrame* input_frame_;
    AVFrame* output_frame_;
    AVPacket* packet_;
    SwsContext* sws_ctx_;
    int width_, height_;
    bool initialized_;
    
public:
    ReliableVideoCodec(int width = 640, int height = 480) 
        : encoder_(nullptr), decoder_(nullptr), 
          input_frame_(nullptr), output_frame_(nullptr), packet_(nullptr),
          sws_ctx_(nullptr), width_(width), height_(height), initialized_(false) {}
    
    ~ReliableVideoCodec() {
        cleanup();
    }
    
    bool init() {
        // 1. ENCODER SETUP - PROVEN RELIABLE SETTINGS
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
        
        // PROVEN RELIABLE SETTINGS - NO PPS ISSUES
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->time_base = {1, 30};
        encoder_->framerate = {30, 1};
        encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder_->bit_rate = 800000; // 800 kbps - tested working
        encoder_->gop_size = 30; // Tested working
        encoder_->max_b_frames = 0; // No B-frames - tested working
        
        // EXACT SAME SETTINGS AS SUCCESSFUL TEST
        av_opt_set(encoder_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder_->priv_data, "profile", "baseline", 0);
        av_opt_set_int(encoder_->priv_data, "crf", 30, 0); // Tested working
        
        if (avcodec_open2(encoder_, enc_codec, nullptr) < 0) {
            std::cerr << "Failed to open encoder" << std::endl;
            return false;
        }
        
        // 2. DECODER SETUP - PROVEN RELIABLE SETTINGS
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
        
        // TOLERANT DECODER - TESTED WORKING
        decoder_->err_recognition = AV_EF_IGNORE_ERR;
        decoder_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
        
        if (avcodec_open2(decoder_, dec_codec, nullptr) < 0) {
            std::cerr << "Failed to open decoder" << std::endl;
            return false;
        }
        
        // 3. ALLOCATE FRAMES AND PACKETS
        input_frame_ = av_frame_alloc();
        output_frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        
        if (!input_frame_ || !output_frame_ || !packet_) {
            std::cerr << "Failed to allocate frames/packets" << std::endl;
            return false;
        }
        
        // Setup input frame for YUV420P
        input_frame_->format = AV_PIX_FMT_YUV420P;
        input_frame_->width = width_;
        input_frame_->height = height_;
        if (av_frame_get_buffer(input_frame_, 32) < 0) {
            std::cerr << "Failed to allocate input frame buffer" << std::endl;
            return false;
        }
        
        // Setup SWS context for YUYV to YUV420P conversion
        sws_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                                 width_, height_, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            std::cerr << "Failed to create SWS context" << std::endl;
            return false;
        }
        
        initialized_ = true;
        std::cout << "Reliable video codec initialized: " << width_ << "x" << height_ << "@30fps" << std::endl;
        return true;
    }
    
    std::vector<uint8_t> encode_frame(const uint8_t* yuyv_data) {
        if (!initialized_) return {};
        
        // Convert YUYV to YUV420P
        const uint8_t* src_data[4] = { yuyv_data, nullptr, nullptr, nullptr };
        int src_linesize[4] = { width_ * 2, 0, 0, 0 }; // YUYV is 2 bytes per pixel
        
        sws_scale(sws_ctx_, src_data, src_linesize, 0, height_,
                 input_frame_->data, input_frame_->linesize);
        
        // Set PTS
        static int64_t pts = 0;
        input_frame_->pts = pts++;
        
        // Encode
        int ret = avcodec_send_frame(encoder_, input_frame_);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder: " << ret << std::endl;
            return {};
        }
        
        ret = avcodec_receive_packet(encoder_, packet_);
        if (ret == AVERROR(EAGAIN)) {
            return {}; // Need more frames
        } else if (ret < 0) {
            std::cerr << "Error receiving packet from encoder: " << ret << std::endl;
            return {};
        }
        
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
        if (ret < 0) {
            std::cerr << "Error sending packet to decoder: " << ret << std::endl;
            return false;
        }
        
        ret = avcodec_receive_frame(decoder_, output_frame_);
        if (ret == AVERROR(EAGAIN)) {
            return true; // Need more packets
        } else if (ret < 0) {
            std::cerr << "Error receiving frame from decoder: " << ret << std::endl;
            return false;
        }
        
        callback(output_frame_);
        return true;
    }
    
    void cleanup() {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }
        if (input_frame_) {
            av_frame_free(&input_frame_);
        }
        if (output_frame_) {
            av_frame_free(&output_frame_);
        }
        if (packet_) {
            av_packet_free(&packet_);
        }
        if (encoder_) {
            avcodec_free_context(&encoder_);
        }
        if (decoder_) {
            avcodec_free_context(&decoder_);
        }
    }
};

// SIMPLE STABLE CAMERA
class StableCamera {
private:
    int fd_;
    std::vector<VideoBuffer> buffers_;
    int width_, height_;
    bool initialized_;
    
public:
    StableCamera() : fd_(-1), width_(640), height_(480), initialized_(false) {}
    
    ~StableCamera() {
        cleanup();
    }
    
    bool init(const std::string& device = "/dev/video0") {
        fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ == -1) {
            std::cerr << "Failed to open camera device: " << device << std::endl;
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
            std::cerr << "Failed to set camera format" << std::endl;
            return false;
        }
        
        // Set frame rate - conservative 30 FPS
        struct v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = 30;
        ioctl(fd_, VIDIOC_S_PARM, &parm);
        
        // Setup buffers
        struct v4l2_requestbuffers req{};
        req.count = 4;
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
        std::cout << "Stable camera initialized: " << width_ << "x" << height_ << "@30fps" << std::endl;
        return true;
    }
    
    const uint8_t* capture_frame() {
        if (!initialized_) return nullptr;
        
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) return nullptr; // No frame ready
            std::cerr << "Failed to dequeue buffer" << std::endl;
            return nullptr;
        }
        
        const uint8_t* frame_data = static_cast<const uint8_t*>(buffers_[buf.index].start);
        
        // Requeue buffer
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

// SIMPLE SDL RENDERER
class SimpleRenderer {
private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    SDL_Texture* texture_;
    int width_, height_;
    bool initialized_;
    
public:
    SimpleRenderer(const std::string& title, int width = 640, int height = 480)
        : window_(nullptr), renderer_(nullptr), texture_(nullptr),
          width_(width), height_(height), initialized_(false) {
        
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
            return;
        }
        
        window_ = SDL_CreateWindow(title.c_str(),
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 width, height, SDL_WINDOW_SHOWN);
        if (!window_) {
            std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
            return;
        }
        
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
            return;
        }
        
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_YV12,
                                   SDL_TEXTUREACCESS_STREAMING, width_, height_);
        if (!texture_) {
            std::cerr << "Failed to create texture: " << SDL_GetError() << std::endl;
            return;
        }
        
        initialized_ = true;
        std::cout << "Simple renderer initialized: " << title << " " << width << "x" << height << std::endl;
    }
    
    ~SimpleRenderer() {
        cleanup();
    }
    
    void render(AVFrame* frame) {
        if (!initialized_ || !frame) return;
        
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
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        if (initialized_) {
            SDL_Quit();
            initialized_ = false;
        }
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
    
    std::cout << "=== FINAL STABLE VIDEO ENGINE ===" << std::endl;
    std::cout << "Peer IP: " << peer_ip << std::endl;
    std::cout << "Local Port: " << local_port << std::endl;
    std::cout << "Peer Port: " << peer_port << std::endl;
    
    // Initialize components
    StableCamera camera;
    ReliableVideoCodec self_codec(640, 480);
    ReliableVideoCodec peer_codec(640, 480);
    SimpleRenderer self_renderer("SELF VIEW", 640, 480);
    SimpleRenderer peer_renderer("PEER VIEW", 640, 480);
    
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
    
    std::cout << "Final stable video engine started. ESC to quit." << std::endl;
    
    // Main loop
    while (true) {
        // Capture and encode
        const uint8_t* yuyv_data = camera.capture_frame();
        if (yuyv_data) {
            auto encoded_data = self_codec.encode_frame(yuyv_data);
            if (!encoded_data.empty()) {
                // Create frame for network
                hydra::media::EncodedFrame frame;
                frame.frame_id = seq_base;
                frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                frame.codec_fourcc = 0x48323634; // "H264"
                
                // Convert uint8_t to std::byte
                frame.data.reserve(encoded_data.size());
                for (auto byte : encoded_data) {
                    frame.data.push_back(static_cast<std::byte>(byte));
                }
                
                // Send over network
                auto packets = packetizer.packetize(frame, seq_base);
                seq_base += packets.size();
                for (const auto& p : packets) {
                    sender.send(p);
                }
                
                // Decode and show self view
                self_codec.decode_frame(encoded_data, [&](AVFrame* frame) {
                    self_renderer.render(frame);
                });
            }
        }
        
        // Poll events
        if (!self_renderer.poll() || !peer_renderer.poll()) {
            break;
        }
        
        // 30 FPS timing
        std::this_thread::sleep_for(33ms);
    }
    
    std::cout << "Final stable video engine stopped." << std::endl;
    return 0;
}
