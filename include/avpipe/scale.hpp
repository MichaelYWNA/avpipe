// SPDX-License-Identifier: MIT
#ifndef AVPIPE_SCALE_HPP
#define AVPIPE_SCALE_HPP

#include <memory>

#include <avpipe/media.hpp>
#include <avpipe/stage.hpp>

namespace avpipe {

/// A FrameSource that rescales/resamples the frames of another FrameSource with libswscale.
///
/// Frames that already match the requested geometry are passed through untouched, so a
/// "scale to the same size" stage costs nothing and can stay in the graph.
class ScalingSource : public FrameSource {
 public:
  static Expected<std::unique_ptr<ScalingSource>> open(FrameSource& source, int width, int height);
  ~ScalingSource() override;

  Expected<Frame> pull() override;

  int64_t frames_converted() const { return converted_; }

 private:
  ScalingSource() = default;

  Expected<Frame> convert(const Frame& source);

  FrameSource* source_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  AVPixelFormat format_ = AV_PIX_FMT_YUV420P;
  SwsContextPtr sws_;
  int64_t converted_ = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_SCALE_HPP
