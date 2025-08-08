#include "hydra/network/NetworkSender.h"

#include <asio.hpp>
#include <stdexcept>

namespace hydra::network {

NetworkSender::NetworkSender(const std::string& remote_address, const std::vector<std::uint16_t>& remote_ports)
    : io_context_(), socket_(io_context_) {
  asio::ip::address address = asio::ip::make_address(remote_address);

  // Open a single UDP IPv4 socket; the OS will choose an ephemeral local port
  socket_.open(asio::ip::udp::v4());

  remote_endpoints_.reserve(remote_ports.size());
  for (std::uint16_t port : remote_ports) {
    remote_endpoints_.emplace_back(address, port);
  }
}

NetworkSender::~NetworkSender() {
  std::error_code ec;
  socket_.close(ec);
}

void NetworkSender::send(const Packet& packet) {
  const auto buffer = asio::const_buffer(&packet, sizeof(Packet));
  std::error_code ec;
  for (const auto& endpoint : remote_endpoints_) {
    socket_.send_to(buffer, endpoint, 0, ec);
    (void)ec; // For now, ignore send errors; callers can add logging later
  }
}

} // namespace hydra::network


