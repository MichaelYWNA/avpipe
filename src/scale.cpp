// SPDX-License-Identifier: MIT
#include <avpipe/scale.hpp>

#include <cerrno>

namespace avpipe {

Expected<std::unique_ptr<ScalingSource>> ScalingSource::open(FrameSource& source, int width,
                                                             int height) {
  if (width <= 0 || height <= 0) {
    return fail(Code::invalid_argument, "ScalingSource::open: non-positive target size");
  }
  std::unique_ptr<ScalingSource> scaler(new ScalingSource());
  scaler->source_ = &source;
  scaler->width_ = width;
  scaler->height_ = height;
  return scaler;
}

ScalingSource::~ScalingSource() = default;

Expected<Frame> ScalingSource::pull() {
  auto input = source_->pull();
  if (!input) {
    return input.error();
  }
  const AVFrame* frame = input.value().get();

  const bool same_size = frame->width == width_ && frame->height == height_;
  const bool same_format = frame->format == static_cast<int>(format_);
  if (same_size && same_format) {
    return input.take();
  }
  return convert(input.value());
}

Expected<Frame> ScalingSource::convert(const Frame& source) {
  Frame out;
  if (auto created = Frame::create(); created) {
    out = created.take();
  } else {
    return created.error();
  }

  const AVFrame* src = source.get();
  out->format = static_cast<int>(format_);
  out->width = width_;
  out->height = height_;
  out->pts = src->pts;
  out->pkt_dts = src->pkt_dts;

  if (const int ret = av_frame_get_buffer(out.get(), 0); ret < 0) {
    return from_av(ret, "av_frame_get_buffer (scale target)");
  }

  // sws_getCachedContext frees the previous context itself, so hand it the raw pointer and
  // take ownership of whatever it returns.
  SwsContext* cached = sws_getCachedContext(
      sws_.release(), src->width, src->height, static_cast<AVPixelFormat>(src->format), width_,
      height_, format_, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (cached == nullptr) {
    return from_av(AVERROR(EINVAL), "sws_getCachedContext");
  }
  sws_.reset(cached);

  const int scaled = sws_scale(sws_.get(), src->data, src->linesize, 0, src->height, out->data,
                               out->linesize);
  if (scaled <= 0) {
    return from_av(AVERROR(EINVAL), "sws_scale produced no lines");
  }
  ++converted_;
  return out;
}

}  // namespace avpipe
