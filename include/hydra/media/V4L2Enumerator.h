#pragma once

#include <cstdint>
#include <string>

namespace hydra::media {

struct CameraMode {
  int width{640};
  int height{360};
  int fps{30};
  std::string pixel_format_fourcc; // e.g., "YUYV", "MJPG"
};

class V4L2Enumerator {
public:
  // Enumerates supported modes and returns the maximum resolution and fps.
  // device_path: e.g., "/dev/video0"
  static CameraMode get_max_mode(const std::string& device_path);
};

} // namespace hydra::media


