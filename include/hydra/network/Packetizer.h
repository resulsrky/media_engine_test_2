#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <unordered_map>

#include "hydra/network/Packet.h"
#include "hydra/media/EncodedFrame.h"

namespace hydra::network {

// Frame to fixed-size Packet(s) with lightweight header extension
// Packet payload layout:
//   [0..7]   frame_id (u64)
//   [8..11]  chunk_id (u32)
//   [12..15] chunk_count (u32)
//   [16..19] codec_fourcc (u32)
//   [20]     flags (bit0: keyframe)
//   [21..23] reserved (pad)
//   [24..31] total_size (u64) -> original frame size in bytes
//   [32..]   payload bytes
struct ChunkHeader {
  std::uint64_t frame_id;
  std::uint32_t chunk_id;
  std::uint32_t chunk_count;
  std::uint32_t codec_fourcc;
  std::uint8_t flags; // bit0 keyframe
  std::uint8_t reserved[3]{};
  std::uint64_t total_size;
};

constexpr std::size_t kPacketPayloadSize = 1184;
constexpr std::size_t kChunkHeaderSize = 32;
constexpr std::size_t kChunkDataSize = kPacketPayloadSize - kChunkHeaderSize; // 1152

class Packetizer {
public:
  std::vector<Packet> packetize(const media::EncodedFrame& frame, std::uint64_t sequence_base);
};

class Depacketizer {
public:
  // Returns a completed EncodedFrame when all chunks of the frame are received.
  // If incomplete, returns nullopt.
  std::optional<media::EncodedFrame> push_and_try_reassemble(const Packet& packet);

private:
  struct Accumulator {
    std::uint64_t frame_id{};
    std::uint32_t expected_chunks{};
    std::uint32_t received_chunks{};
    std::uint32_t codec_fourcc{};
    bool is_keyframe{};
    std::uint64_t timestamp_ns{};
    std::vector<std::byte> data;
    std::vector<bool> received_bitmap;
  };

  std::unordered_map<std::uint64_t, Accumulator> frame_accumulators_;
};

} // namespace hydra::network


