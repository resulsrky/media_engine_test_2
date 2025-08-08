#pragma once

#include <asio.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "hydra/network/Packet.h"

namespace hydra::network {

class NetworkReceiver {
public:
  using PacketCallback = std::function<void(const asio::ip::udp::endpoint& remote, const Packet& packet)>;

  explicit NetworkReceiver(const std::vector<std::uint16_t>& listen_ports);
  ~NetworkReceiver();

  void start(PacketCallback callback);
  void stop();

private:
  struct SocketState {
    explicit SocketState(asio::io_context& io)
        : socket(io) {}
    asio::ip::udp::socket socket;
    asio::ip::udp::endpoint remote_endpoint;
    std::array<std::byte, sizeof(Packet)> buffer{};
  };

  void start_receive(SocketState* state);

  using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

  asio::io_context io_context_;
  std::unique_ptr<WorkGuard> work_guard_;
  std::jthread io_thread_{};

  std::vector<std::unique_ptr<SocketState>> sockets_;
  PacketCallback callback_{};
  std::atomic<bool> running_{false};
};

} // namespace hydra::network


