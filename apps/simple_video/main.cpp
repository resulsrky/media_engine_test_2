#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using namespace std::chrono_literals;

// TEK BİR ENCODER/DECODER ÇİFTİ İLE TEST
class SimpleVideoCodec {
private:
    // Encoder
    AVCodecContext* encoder_;
    AVFrame* input_frame_;
    AVPacket* output_packet_;
    
    // Decoder
    AVCodecContext* decoder_;
    AVFrame* output_frame_;
    AVPacket* input_packet_;
    
    // Common
    int width_, height_;
    bool initialized_;
    
public:
    SimpleVideoCodec(int width = 320, int height = 240) 
        : encoder_(nullptr), decoder_(nullptr), 
          input_frame_(nullptr), output_frame_(nullptr),
          output_packet_(nullptr), input_packet_(nullptr),
          width_(width), height_(height), initialized_(false) {}
    
    ~SimpleVideoCodec() {
        cleanup();
    }
    
    bool init() {
        // 1. ENCODER SETUP
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
        
        // BASIT VE KARARLR ENCODER AYARLARI
        encoder_->width = width_;
        encoder_->height = height_;
        encoder_->time_base = {1, 30};
        encoder_->framerate = {30, 1};
        encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder_->bit_rate = 400000; // Çok düşük bitrate
        encoder_->gop_size = 30; // Her saniye keyframe
        encoder_->max_b_frames = 0; // B frame yok
        
        // EN BASIT AYARLAR - HIÇBIR ÖZEL ÖZELLİK YOK
        av_opt_set(encoder_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder_->priv_data, "profile", "baseline", 0);
        av_opt_set_int(encoder_->priv_data, "crf", 35, 0); // Düşük kalite
        
        if (avcodec_open2(encoder_, enc_codec, nullptr) < 0) {
            std::cerr << "Failed to open encoder" << std::endl;
            return false;
        }
        
        // 2. DECODER SETUP - AYNI PARAMETRELERLE
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
        
        // DECODER: HATA TOLERANSLI
        decoder_->err_recognition = AV_EF_IGNORE_ERR;
        decoder_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
        
        if (avcodec_open2(decoder_, dec_codec, nullptr) < 0) {
            std::cerr << "Failed to open decoder" << std::endl;
            return false;
        }
        
        // 3. FRAME/PACKET ALLOCATION
        input_frame_ = av_frame_alloc();
        output_frame_ = av_frame_alloc();
        output_packet_ = av_packet_alloc();
        input_packet_ = av_packet_alloc();
        
        if (!input_frame_ || !output_frame_ || !output_packet_ || !input_packet_) {
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
        
        initialized_ = true;
        std::cout << "Simple video codec initialized: " << width_ << "x" << height_ << std::endl;
        return true;
    }
    
    // TEST FUNCTION: Dummy frame encode/decode
    bool test_encode_decode() {
        if (!initialized_) return false;
        
        // Create a simple test pattern
        static int frame_counter = 0;
        frame_counter++;
        
        // Fill with a simple pattern
        for (int y = 0; y < height_; y++) {
            for (int x = 0; x < width_; x++) {
                int luma = ((x + y + frame_counter) % 256);
                input_frame_->data[0][y * input_frame_->linesize[0] + x] = luma;
            }
        }
        
        // Fill U and V planes
        for (int y = 0; y < height_/2; y++) {
            for (int x = 0; x < width_/2; x++) {
                input_frame_->data[1][y * input_frame_->linesize[1] + x] = 128;
                input_frame_->data[2][y * input_frame_->linesize[2] + x] = 128;
            }
        }
        
        input_frame_->pts = frame_counter;
        
        // ENCODE
        int ret = avcodec_send_frame(encoder_, input_frame_);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder: " << ret << std::endl;
            return false;
        }
        
        ret = avcodec_receive_packet(encoder_, output_packet_);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                std::cout << "Encoder needs more frames" << std::endl;
                return true; // Not an error
            }
            std::cerr << "Error receiving packet from encoder: " << ret << std::endl;
            return false;
        }
        
        std::cout << "Encoded frame " << frame_counter << ", size: " << output_packet_->size << " bytes" << std::endl;
        
        // DECODE
        ret = avcodec_send_packet(decoder_, output_packet_);
        if (ret < 0) {
            std::cerr << "Error sending packet to decoder: " << ret << std::endl;
            av_packet_unref(output_packet_);
            return false;
        }
        
        ret = avcodec_receive_frame(decoder_, output_frame_);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                std::cout << "Decoder needs more packets" << std::endl;
                av_packet_unref(output_packet_);
                return true; // Not an error
            }
            std::cerr << "Error receiving frame from decoder: " << ret << std::endl;
            av_packet_unref(output_packet_);
            return false;
        }
        
        std::cout << "Decoded frame " << frame_counter << ", format: " << output_frame_->format << std::endl;
        
        av_packet_unref(output_packet_);
        return true;
    }
    
    void cleanup() {
        if (input_frame_) {
            av_frame_free(&input_frame_);
        }
        if (output_frame_) {
            av_frame_free(&output_frame_);
        }
        if (output_packet_) {
            av_packet_free(&output_packet_);
        }
        if (input_packet_) {
            av_packet_free(&input_packet_);
        }
        if (encoder_) {
            avcodec_free_context(&encoder_);
        }
        if (decoder_) {
            avcodec_free_context(&decoder_);
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "=== SIMPLE VIDEO CODEC TEST ===" << std::endl;
    
    // Test basic encode/decode
    SimpleVideoCodec codec(320, 240);
    
    if (!codec.init()) {
        std::cerr << "Failed to initialize codec" << std::endl;
        return -1;
    }
    
    std::cout << "Testing encode/decode loop..." << std::endl;
    
    // Test 100 frames
    for (int i = 0; i < 100; i++) {
        if (!codec.test_encode_decode()) {
            std::cerr << "Test failed at frame " << i << std::endl;
            break;
        }
        
        if (i % 10 == 0) {
            std::cout << "Processed " << i << " frames successfully" << std::endl;
        }
        
        std::this_thread::sleep_for(33ms); // ~30 FPS
    }
    
    std::cout << "\nSimple video codec test completed!" << std::endl;
    return 0;
}
