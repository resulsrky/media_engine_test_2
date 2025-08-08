#include "hydra/media/V4L2Enumerator.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace hydra::media {

static std::string fourcc_to_str(__u32 fourcc) {
  char s[5]{};
  s[0] = fourcc & 0xFF;
  s[1] = (fourcc >> 8) & 0xFF;
  s[2] = (fourcc >> 16) & 0xFF;
  s[3] = (fourcc >> 24) & 0xFF;
  return std::string(s, 4);
}

CameraMode V4L2Enumerator::get_max_mode(const std::string& device_path) {
  int fd = ::open(device_path.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    // Fallback defaults
    return CameraMode{640, 360, 30, "YUYV"};
  }

  CameraMode best{640, 360, 30, "YUYV"};

  v4l2_fmtdesc fmtdesc{};
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (fmtdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0; ++fmtdesc.index) {
    const std::string pf = fourcc_to_str(fmtdesc.pixelformat);
    v4l2_frmsizeenum fsize{};
    fsize.pixel_format = fmtdesc.pixelformat;
    for (fsize.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0; ++fsize.index) {
      int w = 0, h = 0;
      if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        w = fsize.discrete.width;
        h = fsize.discrete.height;
      } else if (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
        w = fsize.stepwise.max_width;
        h = fsize.stepwise.max_height;
      }
      v4l2_frmivalenum fival{};
      fival.pixel_format = fmtdesc.pixelformat;
      fival.width = static_cast<__u32>(w);
      fival.height = static_cast<__u32>(h);
      int max_fps = 0;
      for (fival.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == 0; ++fival.index) {
        if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
          if (fival.discrete.numerator != 0) {
            int fps = static_cast<int>(fival.discrete.denominator / fival.discrete.numerator);
            if (fps > max_fps) max_fps = fps;
          }
        } else if (fival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
          if (fival.stepwise.min.numerator != 0) {
            int fps = static_cast<int>(fival.stepwise.max.denominator / fival.stepwise.min.numerator);
            if (fps > max_fps) max_fps = fps;
          }
        }
      }
      if (w * h > best.width * best.height || (w * h == best.width * best.height && max_fps > best.fps)) {
        best.width = w;
        best.height = h;
        best.fps = (max_fps > 0 ? max_fps : best.fps);
        best.pixel_format_fourcc = pf;
      }
    }
  }

  ::close(fd);
  return best;
}

} // namespace hydra::media


