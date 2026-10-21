// SPDX-License-Identifier: MIT
#include <avpipe/filter.hpp>

#include <cinttypes>
#include <cstdio>

namespace avpipe {

Expected<std::unique_ptr<FilterSource>> FilterSource::open(FrameSource& source,
                                                          const std::string& description,
                                                          AVRational time_base) {
  if (description.empty()) {
    return fail(Code::invalid_argument, "FilterSource::open: empty filter description");
  }
  std::unique_ptr<FilterSource> stage(new FilterSource());
  stage->source_ = &source;
  stage->description_ = description;
  stage->time_base_ = time_base;

  // The graph is built from the real input format, which is only known after one frame.
  auto first = source.pull();
  if (!first) {
    return first.error();
  }
  if (auto status = stage->configure(*first.value().get()); status) {
    return status;
  }
  stage->pending_ = first.take();
  return stage;
}

FilterSource::~FilterSource() = default;

Status FilterSource::configure(const AVFrame& first) {
  graph_.reset(avfilter_graph_alloc());
  if (!graph_) {
    return from_av(AVERROR(ENOMEM), "avfilter_graph_alloc");
  }

  const bool video = first.width > 0 && first.height > 0;
  char args[512];

  if (video) {
    std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                  first.width, first.height, first.format, time_base_.num, time_base_.den,
                  first.sample_aspect_ratio.num > 0 ? first.sample_aspect_ratio.num : 1,
                  first.sample_aspect_ratio.den > 0 ? first.sample_aspect_ratio.den : 1);
    if (avfilter_graph_create_filter(&buffer_src_, avfilter_get_by_name("buffer"), "in", args,
                                     nullptr, graph_.get()) < 0) {
      return fail(Code::ffmpeg, "cannot create buffer source filter");
    }
  } else {
    std::snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%d:channel_layout=0x%" PRIx64,
                  first.sample_rate, first.format,
                  static_cast<std::uint64_t>(first.ch_layout.u.mask));
    if (avfilter_graph_create_filter(&buffer_src_, avfilter_get_by_name("abuffer"), "in", args,
                                     nullptr, graph_.get()) < 0) {
      return fail(Code::ffmpeg, "cannot create abuffer source filter");
    }
  }

  // Audio needs abuffersink, video needs buffersink; using the wrong one fails with a
  // "media type mismatch between ... output pad 0 (audio) and ... input pad 0 (video)".
  if (avfilter_graph_create_filter(&buffer_sink_,
                                   avfilter_get_by_name(video ? "buffersink" : "abuffersink"),
                                   "out", nullptr, nullptr, graph_.get()) < 0) {
    return fail(Code::ffmpeg, "cannot create buffersink filter");
  }

  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (outputs == nullptr || inputs == nullptr) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return from_av(AVERROR(ENOMEM), "avfilter_inout_alloc");
  }
  outputs->name = av_strdup("in");
  outputs->filter_ctx = buffer_src_;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = buffer_sink_;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  const int parsed =
      avfilter_graph_parse_ptr(graph_.get(), description_.c_str(), &inputs, &outputs, nullptr);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (parsed < 0) {
    return from_av(parsed, "cannot parse filter graph '" + description_ + "'");
  }
  if (const int configured = avfilter_graph_config(graph_.get(), nullptr); configured < 0) {
    return from_av(configured, "cannot configure filter graph '" + description_ + "'");
  }
  out_time_base_ = av_buffersink_get_time_base(buffer_sink_);
  if (out_time_base_.num <= 0 || out_time_base_.den <= 0) {
    out_time_base_ = time_base_;
  }
  configured_ = true;
  return ok();
}

Status FilterSource::push(const AVFrame* frame) {
  const int ret = av_buffersrc_add_frame_flags(buffer_src_, const_cast<AVFrame*>(frame),
                                               AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    return from_av(ret, "av_buffersrc_add_frame_flags");
  }
  return ok();
}

Expected<Frame> FilterSource::pull() {
  if (!configured_) {
    return fail(Code::invalid_argument, "FilterSource::pull: graph not configured");
  }

  for (;;) {
    Frame out;
    if (auto created = Frame::create(); created) {
      out = created.take();
    } else {
      return created.error();
    }

    const int ret = av_buffersink_get_frame(buffer_sink_, out.get());
    if (ret == 0) {
      ++frames_out_;
      return out;
    }
    if (ret == AVERROR(EAGAIN)) {
      if (pending_) {
        Frame frame = std::move(pending_);
        pending_.reset();
        if (auto status = push(frame.get()); status) {
          return status;
        }
        continue;
      }
      if (upstream_eof_) {
        if (flushed_) {
          // Flush already sent and the graph still has nothing: it is done.
          return fail(Code::end_of_stream, "filter graph drained");
        }
        flushed_ = true;
        if (auto status = push(nullptr); status) {
          return status;
        }
        continue;
      }
      auto next = source_->pull();
      if (!next) {
        if (next.error().code == Code::end_of_stream) {
          upstream_eof_ = true;
          continue;
        }
        return next.error();
      }
      pending_ = next.take();
      continue;
    }
    if (ret == AVERROR_EOF) {
      return fail(Code::end_of_stream, "filter graph finished");
    }
    return from_av(ret, "av_buffersink_get_frame");
  }
}

}  // namespace avpipe
