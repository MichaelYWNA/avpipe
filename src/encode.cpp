// SPDX-License-Identifier: MIT
#include <avpipe/encode.hpp>

#include <utility>

namespace avpipe {

namespace {

/// Picks a pixel format the encoder accepts, preferring yuv420p because every player handles it
/// and x264/x265 want it anyway.
AVPixelFormat choose_pixel_format(const AVCodec* codec) {
  if (codec->pix_fmts == nullptr) {
    return AV_PIX_FMT_YUV420P;
  }
  for (const AVPixelFormat* it = codec->pix_fmts; *it != AV_PIX_FMT_NONE; ++it) {
    if (*it == AV_PIX_FMT_YUV420P) {
      return *it;
    }
  }
  return codec->pix_fmts[0];
}

}  // namespace

Expected<std::unique_ptr<Encoder>> Encoder::open(const StreamInfo& input,
                                                 const EncodeOptions& options) {
  const AVCodec* codec = avcodec_find_encoder_by_name(options.codec.c_str());
  if (codec == nullptr) {
    return fail(Code::unsupported, "encoder '" + options.codec + "' is not available");
  }
  if (input.type != AVMEDIA_TYPE_VIDEO) {
    return fail(Code::unsupported, "this build only encodes video (input is " +
                                       std::string(av_get_media_type_string(input.type)) + ")");
  }

  std::unique_ptr<Encoder> encoder(new Encoder());
  encoder->context_.reset(avcodec_alloc_context3(codec));
  if (!encoder->context_) {
    return from_av(AVERROR(ENOMEM), "cannot allocate encoder context");
  }
  AVCodecContext* ctx = encoder->context_.get();

  ctx->width = input.params ? input.params->width : 0;
  ctx->height = input.params ? input.params->height : 0;
  if (ctx->width <= 0 || ctx->height <= 0) {
    return fail(Code::invalid_argument, "encoder input has no frame size");
  }
  ctx->pix_fmt = choose_pixel_format(codec);
  // 1/fps, like the ffmpeg CLI: containers pick their own time base (AVI wants 1/600 for mpeg4,
  // mp4 1/15360) and rescaling from a coarse base keeps that conversion exact. Using the source
  // base here instead looks tempting but breaks AVI, where 1/15360 -> 1/600 is a 25.6x factor.
  ctx->time_base = AVRational{1, options.fps};
  ctx->framerate = AVRational{options.fps, 1};
  encoder->source_time_base_ = input.time_base;
  ctx->gop_size = options.gop;
  ctx->max_b_frames = 0;
  ctx->bit_rate = options.bit_rate;
  if (options.threads > 0) {
    ctx->thread_count = options.threads;
  }

  AVDictionary* raw_opts = nullptr;
  if (!options.preset.empty()) {
    av_dict_set(&raw_opts, "preset", options.preset.c_str(), 0);
  }
  if (options.crf >= 0 && !options.codec.empty() && options.codec.rfind("lib", 0) == 0) {
    av_dict_set(&raw_opts, "crf", std::to_string(options.crf).c_str(), 0);
  }

  if (const int ret = avcodec_open2(ctx, codec, &raw_opts); ret < 0) {
    av_dict_free(&raw_opts);
    return from_av(ret, std::string("cannot open encoder ") + codec->name);
  }
  av_dict_free(&raw_opts);

  // Hand back the parameters a muxer needs, derived from the opened context.
  encoder->out_info_.type = AVMEDIA_TYPE_VIDEO;
  encoder->out_info_.stream_index = 0;
  encoder->out_info_.time_base = ctx->time_base;
  encoder->out_info_.params.reset(avcodec_parameters_alloc());
  if (!encoder->out_info_.params) {
    return from_av(AVERROR(ENOMEM), "cannot allocate output codec parameters");
  }
  if (const int ret = avcodec_parameters_from_context(encoder->out_info_.params.get(), ctx);
      ret < 0) {
    return from_av(ret, "cannot export encoder parameters");
  }

  return encoder;
}

Encoder::~Encoder() = default;

std::string Encoder::codec_name() const {
  return context_ ? context_->codec->name : "none";
}

Status Encoder::drain_locked(const char* what) {
  for (;;) {
    Packet packet;
    if (auto created = Packet::create(); created) {
      packet = created.take();
    } else {
      return created.error();
    }

    const int ret = avcodec_receive_packet(context_.get(), packet.get());
    if (ret == 0) {
      ++packets_out_;
      ready_.push_back(std::move(packet));
      continue;
    }
    if (ret == AVERROR_EOF) {
      finished_ = true;
      return ok();
    }
    if (ret == AVERROR(EAGAIN)) {
      return ok();
    }
    return from_av(ret, what);
  }
}

Status Encoder::push(const Frame& frame) {
  if (flushed_) {
    return fail(Code::invalid_argument, "Encoder::push: encoder already flushed");
  }
  if (frame.empty()) {
    return fail(Code::invalid_argument, "Encoder::push: empty frame");
  }

  // A decoded frame carries pts in the *input* stream time base, the encoder counts in its own
  // (1/fps). Without this the output timeline is stretched by tb_in / tb_out - a 2 s clip came
  // out as ~1007 s when the source was mp4 with tb 1/15360.
  Frame rescaled;
  const AVFrame* to_send = frame.get();
  if (source_time_base_.num != context_->time_base.num ||
      source_time_base_.den != context_->time_base.den) {
    auto copy = Frame::clone(frame);
    if (!copy) {
      return copy.error();
    }
    rescaled = copy.take();
    if (rescaled->pts != AV_NOPTS_VALUE) {
      rescaled->pts = av_rescale_q(rescaled->pts, source_time_base_, context_->time_base);
    }
    if (rescaled->pkt_dts != AV_NOPTS_VALUE) {
      rescaled->pkt_dts =
          av_rescale_q(rescaled->pkt_dts, source_time_base_, context_->time_base);
    }
    to_send = rescaled.get();
  }

  for (;;) {
    const int ret = avcodec_send_frame(context_.get(), to_send);
    if (ret == 0) {
      ++frames_in_;
      return drain_locked("avcodec_receive_packet");
    }
    if (ret == AVERROR(EAGAIN)) {
      // Encoder is full: take what it has, then offer the frame again.
      if (auto drained = drain_locked("avcodec_receive_packet"); drained) {
        continue;
      } else {
        return drained;
      }
    }
    return from_av(ret, "avcodec_send_frame");
  }
}

Status Encoder::flush() {
  if (flushed_) {
    return ok();
  }
  flushed_ = true;
  const int ret = avcodec_send_frame(context_.get(), nullptr);
  if (ret < 0 && ret != AVERROR_EOF) {
    return from_av(ret, "cannot start encoder flush");
  }
  return drain_locked("avcodec_receive_packet (flush)");
}

Expected<Packet> Encoder::pull() {
  if (!ready_.empty()) {
    Packet packet = std::move(ready_.front());
    ready_.pop_front();
    return packet;
  }
  if (finished_) {
    return fail(Code::end_of_stream, "encoder finished");
  }
  if (auto drained = drain_locked("avcodec_receive_packet"); drained) {
    if (!ready_.empty()) {
      Packet packet = std::move(ready_.front());
      ready_.pop_front();
      return packet;
    }
    if (finished_) {
      return fail(Code::end_of_stream, "encoder finished");
    }
  } else {
    return drained;
  }
  return fail(Code::need_more, "encoder has no packet yet");
}

}  // namespace avpipe
