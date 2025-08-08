// Hydra - Network Packet definition
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hydra::network {

struct Packet {
  std::uint64_t sequence_number{0};
  std::uint64_t timestamp_ns{0};
  std::array<std::byte, 1184> payload{}; // Total size: 1200 bytes
};

static_assert(sizeof(Packet) == 1200, "Packet must be exactly 1200 bytes");

} // namespace hydra::network


