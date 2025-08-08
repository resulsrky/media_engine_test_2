#include <cstring>
#include <algorithm>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "hydra/network/NetworkReceiver.h"

using hydra::network::NetworkReceiver;
using hydra::network::Packet;

int main(int argc, char** argv) {
  std::vector<std::uint16_t> ports{5000, 5001, 5002};
  if (argc >= 2) {
    ports.clear();
    for (int i = 1; i < argc; ++i) {
      ports.push_back(static_cast<std::uint16_t>(std::stoi(argv[i])));
    }
  }

  std::cout << "Receiver portlar: ";
  for (auto p : ports) std::cout << p << ' ';
  std::cout << std::endl;

  NetworkReceiver receiver(ports);
  receiver.start([](const asio::ip::udp::endpoint& remote, const Packet& packet) {
    std::string text;
    text.resize(16);
    std::memcpy(text.data(), packet.payload.data(), std::min<std::size_t>(text.size(), packet.payload.size()));
    std::cout << "Aldi: seq=" << packet.sequence_number << ", ts(ns)=" << packet.timestamp_ns
              << ", from=" << remote.address().to_string() << ':' << remote.port()
              << ", payload[0..15]='" << text << "'" << std::endl;
  });

  std::cout << "Dinleniyor... (Crtl+C ile cikis)\n";
  // Ana thread'i blokla
  std::promise<void> wait_forever;
  wait_forever.get_future().wait();

  return 0;
}


