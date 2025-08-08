#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <future>

#include "hydra/media/FFmpegCpuPipeline.h"
#include "hydra/media/FFmpegDecoder.h"
#include "hydra/media/SdlRenderer.h"
#include "hydra/network/NetworkSender.h"
#include "hydra/network/NetworkReceiver.h"
#include "hydra/network/Packetizer.h"

// Kullanım:
// duplex_ffmpeg_app <remote_ip> <send_ports...> --listen <recv_ports...>
// Örnek: duplex_ffmpeg_app 192.168.1.10 7000 7001 7002 --listen 7000 7001 7002

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cout << "Kullanim: duplex_ffmpeg_app <remote_ip> <send_ports...> --listen <recv_ports...>\n";
    return 1;
  }
  std::string remote_ip = argv[1];
  std::vector<std::uint16_t> send_ports;
  std::vector<std::uint16_t> recv_ports;
  bool listen_mode = false;
  for (int i = 2; i < argc; ++i) {
    std::string tok = argv[i];
    if (tok == "--listen") { listen_mode = true; continue; }
    if (!listen_mode) send_ports.push_back(static_cast<std::uint16_t>(std::stoi(tok)));
    else recv_ports.push_back(static_cast<std::uint16_t>(std::stoi(tok)));
  }
  if (send_ports.empty() || recv_ports.empty()) {
    std::cout << "Hata: gonderim ve dinleme portlarini belirtin.\n";
    return 2;
  }

  hydra::network::NetworkSender sender(remote_ip, send_ports);
  hydra::network::NetworkReceiver receiver(recv_ports);
  hydra::network::Packetizer packetizer;
  hydra::network::Depacketizer depacketizer;

  hydra::media::FFmpegCpuEncodingPipeline encoder(640, 360, 30);
  hydra::media::FFmpegDecoder decoder;
  hydra::media::SdlRenderer renderer;
  renderer.open(640, 360, "Hydra Duplex");

  std::atomic<bool> running{true};

  // Receive path
  receiver.start([&](const asio::ip::udp::endpoint&, const hydra::network::Packet& pkt) {
    auto frame_opt = depacketizer.push_and_try_reassemble(pkt);
    if (frame_opt) {
      decoder.push(*frame_opt, [&](const hydra::media::DecodedFrame& df){
        renderer.render(df);
      });
    }
  });

  // Send path
  std::uint64_t seq_base = 0;
  encoder.start([&](const hydra::media::EncodedFrame& frame){
    auto packets = packetizer.packetize(frame, seq_base);
    seq_base += packets.size();
    for (const auto& p : packets) sender.send(p);
  });

  std::cout << "Duplex basladi. Karsi taraf da ayni uygulamayi calistirmali. (Ctrl+C ile cikis)\n";
  while (running) {
    renderer.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 0;
}


