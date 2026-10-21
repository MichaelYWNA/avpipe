// SPDX-License-Identifier: MIT
#ifndef AVPIPE_FILTER_HPP
#define AVPIPE_FILTER_HPP

#include <memory>
#include <string>

#include <avpipe/media.hpp>
#include <avpipe/stage.hpp>

namespace avpipe {

/// libavfilter stage: inserts an arbitrary filter chain (e.g. "fps=15,scale=640:480,crop=...")
/// between two frame stages.
///
/// The graph needs the input format before it can be built, so the first frame is consumed at
/// construction time and replayed on the first `pull()`. Without a filter chain this stage would
/// be pointless - with one, frame-rate conversion (which the plain scaler cannot do) becomes
/// possible, because `fps` is a real filter.
class FilterSource : public FrameSource {
 public:
  static Expected<std::unique_ptr<FilterSource>> open(FrameSource& source,
                                                      const std::string& description,
                                                      AVRational time_base);
  ~FilterSource() override;

  Expected<Frame> pull() override;

  std::string description() const { return description_; }
  /// Time base of the frames this stage emits. It is *not* the source time base: `fps=15` makes
  /// the graph emit frames on a 1/15 grid, and pretending otherwise makes the encoder rescale
  /// them a second time (0,1,2 -> 0,0,1) and reject the stream.
  AVRational output_time_base() const { return out_time_base_; }

 private:
  FilterSource() = default;

  Status configure(const AVFrame& first);
  Status push(const AVFrame* frame);

  FrameSource* source_ = nullptr;
  std::string description_;
  AVRational time_base_{1, 30};
  FilterGraphPtr graph_;
  AVFilterContext* buffer_src_ = nullptr;
  AVFilterContext* buffer_sink_ = nullptr;
  AVRational out_time_base_{1, 30};
  Frame pending_;
  bool configured_ = false;
  bool upstream_eof_ = false;
  bool flushed_ = false;
  int64_t frames_out_ = 0;
};

}  // namespace avpipe

#endif  // AVPIPE_FILTER_HPP
