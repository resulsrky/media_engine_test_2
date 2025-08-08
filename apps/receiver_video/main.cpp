#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <future>

#include "hydra/network/NetworkReceiver.h"
#include "hydra/network/Packetizer.h"

using hydra::network::NetworkReceiver;
using hydra::network::Depacketizer;
using hydra::network::ChunkHeader;

int main(int argc, char** argv) {
  std::vector<std::uint16_t> ports{6000, 6001, 6002};
  if (argc >= 2) {
    ports.clear();
    for (int i = 1; i < argc; ++i) ports.push_back(static_cast<std::uint16_t>(std::stoi(argv[i])));
  }

  Depacketizer depacketizer;

  NetworkReceiver receiver(ports);
  receiver.start([&](const asio::ip::udp::endpoint& remote, const hydra::network::Packet& packet) {
    auto frame_opt = depacketizer.push_and_try_reassemble(packet);
    if (frame_opt) {
      const auto& frame = *frame_opt;
      std::cout << "Tamamlandi: frame_id=" << frame.frame_id
                << ", size=" << frame.data.size()
                << ", key=" << (frame.is_keyframe ? 1 : 0)
                << ", from=" << remote.address().to_string() << ":" << remote.port()
                << std::endl;
    }
  });

  std::cout << "Video Receiver dinleniyor... (Ctrl+C)\n";
  std::promise<void> wait_forever;
  wait_forever.get_future().wait();
  return 0;
}


