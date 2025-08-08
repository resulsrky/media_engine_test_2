#include "hydra/network/Packetizer.h"

#include <algorithm>
#include <cstring>

namespace hydra::network {

std::vector<Packet> Packetizer::packetize(const media::EncodedFrame& frame, std::uint64_t sequence_base) {
  const std::size_t data_size = frame.data.size();
  const std::uint32_t chunk_count = static_cast<std::uint32_t>((data_size + kChunkDataSize - 1) / kChunkDataSize);

  std::vector<Packet> packets;
  packets.reserve(chunk_count);

  for (std::uint32_t chunk_id = 0; chunk_id < chunk_count; ++chunk_id) {
    Packet pkt{};
    pkt.sequence_number = sequence_base + chunk_id;
    pkt.timestamp_ns = frame.timestamp_ns;

    ChunkHeader header{};
    header.frame_id = frame.frame_id;
    header.chunk_id = chunk_id;
    header.chunk_count = chunk_count;
    header.codec_fourcc = frame.codec_fourcc;
    header.flags = static_cast<std::uint8_t>(frame.is_keyframe ? 0x01 : 0x00);
    header.total_size = static_cast<std::uint64_t>(data_size);

    // write header
    std::memcpy(pkt.payload.data(), &header, sizeof(ChunkHeader));

    const std::size_t offset = static_cast<std::size_t>(chunk_id) * kChunkDataSize;
    const std::size_t to_copy = std::min<std::size_t>(kChunkDataSize, data_size - offset);
    if (to_copy > 0) {
      std::memcpy(pkt.payload.data() + kChunkHeaderSize, frame.data.data() + offset, to_copy);
    }
    packets.push_back(pkt);
  }
  return packets;
}

std::optional<media::EncodedFrame> Depacketizer::push_and_try_reassemble(const Packet& packet) {
  // parse header
  ChunkHeader header{};
  std::memcpy(&header, packet.payload.data(), sizeof(ChunkHeader));

  auto& acc = frame_accumulators_[header.frame_id];
  if (acc.received_chunks == 0) {
    acc.frame_id = header.frame_id;
    acc.expected_chunks = header.chunk_count;
    acc.codec_fourcc = header.codec_fourcc;
    acc.is_keyframe = (header.flags & 0x01) != 0;
    acc.timestamp_ns = packet.timestamp_ns;
    acc.data.resize(static_cast<std::size_t>(header.total_size));
    acc.received_bitmap.assign(acc.expected_chunks, false);
  }

  if (header.chunk_id < acc.expected_chunks && !acc.received_bitmap[header.chunk_id]) {
    const std::size_t offset = static_cast<std::size_t>(header.chunk_id) * kChunkDataSize;
    const std::size_t remaining = acc.data.size() - std::min(acc.data.size(), offset);
    const std::size_t to_copy = std::min<std::size_t>(kChunkDataSize, remaining);
    if (to_copy > 0) {
      std::memcpy(acc.data.data() + offset, packet.payload.data() + kChunkHeaderSize, to_copy);
    }
    acc.received_bitmap[header.chunk_id] = true;
    ++acc.received_chunks;
  }

  if (acc.received_chunks == acc.expected_chunks) {
    media::EncodedFrame frame{};
    frame.frame_id = acc.frame_id;
    frame.timestamp_ns = acc.timestamp_ns;
    frame.codec_fourcc = acc.codec_fourcc;
    frame.is_keyframe = acc.is_keyframe;
    frame.data = std::move(acc.data);
    frame_accumulators_.erase(header.frame_id);
    return frame;
  }

  return std::nullopt;
}

} // namespace hydra::network


