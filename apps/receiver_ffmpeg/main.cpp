#include <iostream>
#include <string>
#include <vector>
#include <future>

#include "hydra/network/NetworkReceiver.h"
#include "hydra/network/Packetizer.h"

int main(int argc, char** argv) {
  std::vector<std::uint16_t> ports{7000, 7001, 7002};
  if (argc >= 2) {
    ports.clear();
    for (int i = 1; i < argc; ++i) ports.push_back(static_cast<std::uint16_t>(std::stoi(argv[i])));
  }

  hydra::network::Depacketizer depacketizer;
  hydra::network::NetworkReceiver receiver(ports);
  receiver.start([&](const asio::ip::udp::endpoint& remote, const hydra::network::Packet& pkt){
    auto frame = depacketizer.push_and_try_reassemble(pkt);
    if (frame) {
      std::cout << "FFmpeg alindi frame_id=" << frame->frame_id << ", size=" << frame->data.size() << std::endl;
    }
  });

  std::cout << "FFmpeg receiver calisiyor... (Ctrl+C)\n";
  std::promise<void> wait_forever;
  wait_forever.get_future().wait();
  return 0;
}


