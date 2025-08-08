#pragma once

#include <cstdint>
#include <vector>

namespace hydra::media {

enum class PixelFormat {
  Unknown = 0,
  YUV420P,
};

struct DecodedFrame {
  std::uint64_t frame_id{0};
  std::uint64_t timestamp_ns{0};
  int width{0};
  int height{0};
  PixelFormat format{PixelFormat::YUV420P};
  // I420 layout planes
  std::vector<std::uint8_t> plane_y;
  std::vector<std::uint8_t> plane_u;
  std::vector<std::uint8_t> plane_v;
};

} // namespace hydra::media


