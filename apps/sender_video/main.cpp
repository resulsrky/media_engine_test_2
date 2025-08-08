#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "hydra/network/NetworkSender.h"
#include "hydra/network/Packetizer.h"
#include "hydra/media/EncodedFrame.h"

using hydra::network::NetworkSender;
using hydra::network::Packetizer;
using hydra::media::EncodedFrame;

int main(int argc, char** argv) {
  std::string remote_ip = "127.0.0.1";
  if (argc >= 2) remote_ip = argv[1];
  std::vector<std::uint16_t> ports{6000, 6001, 6002};
  if (argc >= 3) {
    ports.clear();
    for (int i = 2; i < argc; ++i) ports.push_back(static_cast<std::uint16_t>(std::stoi(argv[i])));
  }

  std::cout << "Video Sender hedef IP: " << remote_ip << ", portlar: ";
  for (auto p : ports) std::cout << p << ' ';
  std::cout << std::endl;

  NetworkSender sender(remote_ip, ports);
  Packetizer packetizer;

  std::uint64_t seq_base = 0;
  std::uint64_t frame_id = 0;

  while (true) {
    // Demo: sahte bir video frame (ör. 2.5 KB) üretip gönderelim
    EncodedFrame frame{};
    frame.frame_id = frame_id++;
    frame.timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    frame.codec_fourcc = hydra::media::make_fourcc('R','A','W',' ');
    frame.is_keyframe = (frame.frame_id % 30 == 0);
    frame.data.resize(2560);
    std::memset(frame.data.data(), 0, frame.data.size());
    const char* demo = "FRAME_DEMO_DATA";
    std::memcpy(frame.data.data(), demo, std::min<std::size_t>(frame.data.size(), std::strlen(demo)));

    auto packets = packetizer.packetize(frame, seq_base);
    seq_base += packets.size();
    for (const auto& p : packets) {
      sender.send(p);
    }

    std::cout << "Gonderildi frame_id=" << frame.frame_id << ", paket_sayisi=" << packets.size() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
  }

  return 0;
}


