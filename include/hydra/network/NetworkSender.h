#pragma once

#include <asio.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "hydra/network/Packet.h"

namespace hydra::network {

class NetworkSender {
public:
  NetworkSender(const std::string& remote_address, const std::vector<std::uint16_t>& remote_ports);
  ~NetworkSender();

  void send(const Packet& packet);

private:
  asio::io_context io_context_;
  asio::ip::udp::socket socket_;
  std::vector<asio::ip::udp::endpoint> remote_endpoints_;
};

} // namespace hydra::network


