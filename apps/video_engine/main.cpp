#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

#include "hydra/media/V4L2Enumerator.h"
#include "hydra/media/FFmpegCameraEncodingPipeline.h"
#include "hydra/media/FFmpegDecoder.h"
#include "hydra/media/SdlRenderer.h"
#include "hydra/network/NetworkSender.h"
#include "hydra/network/NetworkReceiver.h"
#include "hydra/network/Packetizer.h"

std::vector<std::uint16_t> parse_ports(const std::string& port_str) {
  std::vector<std::uint16_t> ports;
  std::stringstream ss(port_str);
  std::string port;
  while (std::getline(ss, port, ',')) {
    ports.push_back(static_cast<std::uint16_t>(std::stoi(port)));
  }
  return ports;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cout << "Kullanim: video_engine <peer_ip> <local_ports> [peer_ports] [device=/dev/video0]" << std::endl;
    std::cout << "Ornek: video_engine 192.168.1.5 7000,7001,7002 7010,7011,7012 /dev/video0" << std::endl;
    std::cout << "Not: local_ports = dinlenecek portlar, peer_ports = gonderilecek portlar" << std::endl;
    return 1;
  }
  
  std::string peer_ip = argv[1];
  std::string local_ports_str = argv[2];
  std::string peer_ports_str = (argc >= 4) ? argv[3] : local_ports_str;
  std::string device = (argc >= 5) ? argv[4] : std::string("/dev/video0");

  // Portları parse et
  auto local_ports = parse_ports(local_ports_str);
  auto peer_ports = parse_ports(peer_ports_str);

  // Kamera max modunu bul
  auto mode = hydra::media::V4L2Enumerator::get_max_mode(device);
  std::cout << "Kamera: " << device << " => " << mode.width << "x" << mode.height << "@" << mode.fps
            << " pixfmt=" << mode.pixel_format_fourcc << std::endl;

  // Ağ kur
  hydra::network::NetworkSender sender(peer_ip, peer_ports);
  hydra::network::NetworkReceiver receiver(local_ports);
  hydra::network::Packetizer packetizer;
  hydra::network::Depacketizer depacketizer;

  // Medya kur - İki farklı renderer
  hydra::media::FFmpegCameraEncodingPipeline cam(device, mode.width, mode.height, mode.fps);
  hydra::media::FFmpegDecoder decoder;
  
  // Kendi görüntünüz için renderer
  hydra::media::SdlRenderer self_renderer;
  self_renderer.open(mode.width, mode.height, "Kendi Goruntum");
  
  // Arkadaşınızın görüntüsü için renderer
  hydra::media::SdlRenderer peer_renderer;
  peer_renderer.open(mode.width, mode.height, "Arkadasim - " + peer_ip);

  // Receive → Decode → Render (Arkadaşınızın görüntüsü)
  receiver.start([&](const asio::ip::udp::endpoint&, const hydra::network::Packet& pkt) {
    auto frame_opt = depacketizer.push_and_try_reassemble(pkt);
    if (frame_opt) {
      decoder.push(*frame_opt, [&](const hydra::media::DecodedFrame& df){
        peer_renderer.render(df);
      });
    }
  });

  // Send → Packetize → Network (Kendi görüntünüz)
  std::uint64_t seq_base = 0;
  cam.start([&](const hydra::media::EncodedFrame& frame){
    auto packets = packetizer.packetize(frame, seq_base);
    seq_base += packets.size();
    for (const auto& p : packets) sender.send(p);
    
    // Kendi görüntünüzü de decode edip göster
    decoder.push(frame, [&](const hydra::media::DecodedFrame& df){
      self_renderer.render(df);
    });
  });

  std::cout << "=== HYDRA VIDEO ENGINE - DUPLEX MODE ===" << std::endl;
  std::cout << "Peer IP: " << peer_ip << std::endl;
  std::cout << "Local ports (dinleme): ";
  for (auto p : local_ports) std::cout << p << " ";
  std::cout << std::endl;
  std::cout << "Peer ports (gonderim): ";
  for (auto p : peer_ports) std::cout << p << " ";
  std::cout << std::endl;
  std::cout << "Iki pencere acildi: 'Kendi Goruntum' ve 'Arkadasim - " << peer_ip << "'" << std::endl;
  std::cout << "Goruntu akisi basladi. Cikmak icin Ctrl+C." << std::endl;

  // Ana döngü - Her iki renderer'ı da poll et
  while (true) {
    self_renderer.poll();
    peer_renderer.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}


