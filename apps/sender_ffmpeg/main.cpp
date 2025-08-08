#include <iostream>
#include <string>
#include <vector>

#include "hydra/media/FFmpegCpuPipeline.h"
#include "hydra/network/Packetizer.h"
#include "hydra/network/NetworkSender.h"

using hydra::media::FFmpegCpuEncodingPipeline;
using hydra::network::Packetizer;
using hydra::network::NetworkSender;

int main(int argc, char** argv) {
  std::string remote_ip = "127.0.0.1";
  if (argc >= 2) remote_ip = argv[1];
  std::vector<std::uint16_t> ports{7000, 7001, 7002};
  if (argc >= 3) {
    ports.clear();
    for (int i = 2; i < argc; ++i) ports.push_back(static_cast<std::uint16_t>(std::stoi(argv[i])));
  }

  NetworkSender sender(remote_ip, ports);
  Packetizer packetizer;
  FFmpegCpuEncodingPipeline pipeline(640, 360, 30);

  std::uint64_t seq_base = 0;
  pipeline.start([&](const hydra::media::EncodedFrame& frame){
    auto packets = packetizer.packetize(frame, seq_base);
    seq_base += packets.size();
    for (const auto& p : packets) sender.send(p);
    std::cout << "FFmpeg gonderildi frame_id=" << frame.frame_id << ", pkts=" << packets.size() << std::endl;
  });

  std::cout << "FFmpeg sender calisiyor... (Ctrl+C)\n";
  std::promise<void> wait_forever;
  wait_forever.get_future().wait();
  return 0;
}


