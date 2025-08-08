#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "hydra/media/V4L2Enumerator.h"
#include "hydra/media/FFmpegCameraEncodingPipeline.h"
#include "hydra/media/FFmpegDecoder.h"
#include "hydra/media/SdlRenderer.h"
#include "hydra/network/NetworkSender.h"
#include "hydra/network/NetworkReceiver.h"
#include "hydra/network/Packetizer.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "Kullanim: video_engine <peer_ip> [device=/dev/video0]" << std::endl;
    return 1;
  }
  std::string peer_ip = argv[1];
  std::string device = (argc >= 3) ? argv[2] : std::string("/dev/video0");

  // Çoklu port listesi sabit, gerekirse config’e taşınır
  std::vector<std::uint16_t> ports{7000, 7001, 7002};

  // Kamera max modunu bul
  auto mode = hydra::media::V4L2Enumerator::get_max_mode(device);
  std::cout << "Kamera: " << device << " => " << mode.width << "x" << mode.height << "@" << mode.fps
            << " pixfmt=" << mode.pixel_format_fourcc << std::endl;

  // Ağ kur
  hydra::network::NetworkSender sender(peer_ip, ports);
  hydra::network::NetworkReceiver receiver(ports);
  hydra::network::Packetizer packetizer;
  hydra::network::Depacketizer depacketizer;

  // Medya kur
  hydra::media::FFmpegCameraEncodingPipeline cam(device, mode.width, mode.height, mode.fps);
  hydra::media::FFmpegDecoder decoder;
  hydra::media::SdlRenderer renderer;
  renderer.open(mode.width, mode.height, "Hydra Video Engine");

  // Receive → Decode → Render
  receiver.start([&](const asio::ip::udp::endpoint&, const hydra::network::Packet& pkt) {
    auto frame_opt = depacketizer.push_and_try_reassemble(pkt);
    if (frame_opt) {
      decoder.push(*frame_opt, [&](const hydra::media::DecodedFrame& df){
        renderer.render(df);
      });
    }
  });

  // Send → Packetize → Network
  std::uint64_t seq_base = 0;
  cam.start([&](const hydra::media::EncodedFrame& frame){
    auto packets = packetizer.packetize(frame, seq_base);
    seq_base += packets.size();
    for (const auto& p : packets) sender.send(p);
  });

  std::cout << "Peer IP: " << peer_ip << " | Portlar: 7000 7001 7002" << std::endl;
  std::cout << "Goruntu akisi basladi. Karsi taraf da ayni komutu kendi peer IP'nizle calistirmali." << std::endl;

  while (true) {
    renderer.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}


