#include "hydra/network/NetworkReceiver.h"

#include <asio.hpp>
#include <cstring>

namespace hydra::network {

NetworkReceiver::NetworkReceiver(const std::vector<std::uint16_t>& listen_ports) {
  sockets_.reserve(listen_ports.size());
  for (std::uint16_t port : listen_ports) {
    auto state = std::make_unique<SocketState>(io_context_);
    state->socket.open(asio::ip::udp::v4());
    state->socket.set_option(asio::socket_base::reuse_address(true));
    state->socket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port));
    sockets_.push_back(std::move(state));
  }
}

NetworkReceiver::~NetworkReceiver() { stop(); }

void NetworkReceiver::start(PacketCallback callback) {
  if (running_.exchange(true)) {
    return; // already running
  }

  callback_ = std::move(callback);
  work_guard_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_context_));

  for (auto& state : sockets_) {
    start_receive(state.get());
  }

  io_thread_ = std::jthread([this] {
    io_context_.run();
  });
}

void NetworkReceiver::stop() {
  if (!running_.exchange(false)) {
    return; // already stopped
  }

  std::error_code ec;
  for (auto& state : sockets_) {
    state->socket.cancel(ec);
    state->socket.close(ec);
  }

  if (work_guard_) {
    work_guard_.reset();
  }

  io_context_.stop();

  if (io_thread_.joinable()) {
    io_thread_.join();
  }

  io_context_.restart();
}

void NetworkReceiver::start_receive(SocketState* state) {
  state->socket.async_receive_from(
      asio::buffer(state->buffer.data(), state->buffer.size()),
      state->remote_endpoint,
      [this, state](const std::error_code& ec, std::size_t bytes_received) {
        if (!running_) {
          return;
        }
        if (!ec && bytes_received == sizeof(Packet)) {
          Packet packet;
          std::memcpy(&packet, state->buffer.data(), sizeof(Packet));
          if (callback_) {
            callback_(state->remote_endpoint, packet);
          }
        }
        if (!ec || ec == asio::error::operation_aborted) {
          // Continue receiving unless stopped
          start_receive(state);
        }
      });
}

} // namespace hydra::network


