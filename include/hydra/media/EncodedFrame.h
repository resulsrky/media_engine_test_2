#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <span>

namespace hydra::media {

// Simple FourCC helper for codec identification (e.g., 'H264', 'AV1 ', 'RAW ')
constexpr std::uint32_t make_fourcc(char a, char b, char c, char d) {
  return (static_cast<std::uint32_t>(a) << 24) |
         (static_cast<std::uint32_t>(b) << 16) |
         (static_cast<std::uint32_t>(c) << 8) |
         (static_cast<std::uint32_t>(d) << 0);
}

struct EncodedFrame {
  std::uint64_t frame_id{0};
  std::uint64_t timestamp_ns{0};
  std::uint32_t codec_fourcc{make_fourcc('R','A','W',' ')}; // default RAW
  bool is_keyframe{false};
  std::vector<std::byte> data{};
};

using EncodedFrameView = std::span<const std::byte>;

} // namespace hydra::media


