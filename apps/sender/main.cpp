#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "hydra/network/NetworkSender.h"

using hydra::network::NetworkSender;
using hydra::network::Packet;

int main(int argc, char** argv) {
  std::string remote_ip = "127.0.0.1";
  if (argc >= 2) {
    remote_ip = argv[1];
  }
  std::vector<std::uint16_t> ports{5000, 5001, 5002};
  if (argc >= 3) {
    ports.clear();
    for (int i = 2; i < argc; ++i) {
      ports.push_back(static_cast<std::uint16_t>(std::stoi(argv[i])));
    }
  }

  std::cout << "Sender hedef IP: " << remote_ip << ", portlar: ";
  for (auto p : ports) std::cout << p << ' ';
  std::cout << std::endl;

  NetworkSender sender(remote_ip, ports);

  std::uint64_t sequence = 0;
  while (true) {
    Packet packet{};
    packet.sequence_number = sequence++;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    packet.timestamp_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    const char* msg = "Merhaba Dunya"; // ASCII for portability in terminals
    std::memset(packet.payload.data(), 0, packet.payload.size());
    std::memcpy(packet.payload.data(), msg, std::min<std::size_t>(std::strlen(msg), packet.payload.size()));

    sender.send(packet);

    std::cout << "Gonderildi: seq=" << packet.sequence_number << ", ts(ns)=" << packet.timestamp_ns << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}


