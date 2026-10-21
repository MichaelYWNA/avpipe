// SPDX-License-Identifier: MIT
#include <avpipe/encode.hpp>

#include <utility>

namespace avpipe {

namespace {

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

/// Maps a software encoder name to its hardware counterpart: libx264 + vaapi -> h264_vaapi.
std::string hardware_codec_name(const std::string& codec, const std::string& device) {
  if (codec.find("_vaapi") != std::string::npos || codec.find("_nvenc") != std::string::npos ||
      codec.find("_qsv") != std::string::npos) {
    return codec;
  }
  const bool hevc = codec.find("265") != std::string::npos || codec == "hevc";
  const bool h264 = !hevc;
  if (device == "vaapi") {
    return h264 ? "h264_vaapi" : "hevc_vaapi";
  }
  if (device == "cuda") {
    return h264 ? "h264_nvenc" : "hevc_nvenc";
  }
  return codec + "_" + device;
}

}  // namespace

Expected<std::unique_ptr<Encoder>> Encoder::open(const StreamInfo& input,
                                                 const EncodeOptions& options) {
  std::unique_ptr<Encoder> encoder(new Encoder());

  Status status;
  if (input.type == AVMEDIA_TYPE_AUDIO) {
    status = encoder->open_audio(input, options);
  } else if (input.type == AVMEDIA_TYPE_VIDEO) {
    status = encoder->open_video(input, options);
  } else {
    return fail(Code::unsupported, "this build encodes video and audio only");
  }
  if (status) {
    return status;
  }

  encoder->out_info_.params.reset(avcodec_parameters_alloc());
  if (!encoder->out_info_.params) {
    return from_av(AVERROR(ENOMEM), "cannot allocate output codec parameters");
  }
  if (const int ret =
          avcodec_parameters_from_context(encoder->out_info_.params.get(), encoder->context_.get());
      ret < 0) {
    return from_av(ret, "cannot export encoder parameters");
  }
  return encoder;
}

Status Encoder::open_video(const StreamInfo& input, const EncodeOptions& options) {
  // Validate the device name first: otherwise "hw=nosuchdev" turns into the misleading
  // "encoder 'libx264_nosuchdev' is not available".
  if (!options.hw_device.empty() &&
      av_hwdevice_find_type_by_name(options.hw_device.c_str()) == AV_HWDEVICE_TYPE_NONE) {
    return fail(Code::unsupported, "unknown hardware device type '" + options.hw_device + "'");
  }
  const std::string codec_name =
      options.hw_device.empty() ? options.codec
                                : hardware_codec_name(options.codec, options.hw_device);
  const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
  if (codec == nullptr) {
    return fail(Code::unsupported, "encoder '" + codec_name + "' is not available");
  }

  context_.reset(avcodec_alloc_context3(codec));
  if (!context_) {
    return from_av(AVERROR(ENOMEM), "cannot allocate encoder context");
  }
  AVCodecContext* ctx = context_.get();

  ctx->width = input.params ? input.params->width : 0;
  ctx->height = input.params ? input.params->height : 0;
  if (ctx->width <= 0 || ctx->height <= 0) {
    return fail(Code::invalid_argument, "encoder input has no frame size");
  }
  ctx->pix_fmt = choose_pixel_format(codec);
  // 1/fps, like the ffmpeg CLI: containers pick their own time base (AVI wants 1/600 for mpeg4,
  // mp4 1/15360) and rescaling from a coarse base keeps that conversion exact.
  ctx->time_base = AVRational{1, options.fps};
  ctx->framerate = AVRational{options.fps, 1};
  ctx->gop_size = options.gop;
  ctx->max_b_frames = 0;
  ctx->bit_rate = options.bit_rate;
  if (options.threads > 0) {
    ctx->thread_count = options.threads;
  }
  source_time_base_ = input.time_base;

  out_info_.type = AVMEDIA_TYPE_VIDEO;
  out_info_.stream_index = 0;
  out_info_.time_base = ctx->time_base;

  if (!options.hw_device.empty()) {
    if (auto status = setup_hardware(options); status) {
      return status;
    }
  }

  AVDictionary* raw_opts = nullptr;
  if (!options.preset.empty()) {
    av_dict_set(&raw_opts, "preset", options.preset.c_str(), 0);
  }
  if (options.crf >= 0 && options.codec.rfind("lib", 0) == 0) {
    av_dict_set(&raw_opts, "crf", std::to_string(options.crf).c_str(), 0);
  }
  const int ret = avcodec_open2(ctx, codec, &raw_opts);
  av_dict_free(&raw_opts);
  if (ret < 0) {
    return from_av(ret, std::string("cannot open encoder ") + codec->name);
  }
  return ok();
}

Status Encoder::setup_hardware(const EncodeOptions& options) {
  const AVHWDeviceType type = av_hwdevice_find_type_by_name(options.hw_device.c_str());
  if (type == AV_HWDEVICE_TYPE_NONE) {
    return fail(Code::unsupported, "unknown hardware device type '" + options.hw_device + "'");
  }

  AVBufferRef* device = nullptr;
  const char* device_name =
      options.hw_device_name.empty() ? nullptr : options.hw_device_name.c_str();
  if (const int ret = av_hwdevice_ctx_create(&device, type, device_name, nullptr, 0); ret < 0) {
    return from_av(ret, "cannot open " + options.hw_device + " device" +
                            (options.hw_device_name.empty() ? "" : " " + options.hw_device_name));
  }
  hw_device_ctx_.reset(device);
  context_->hw_device_ctx = av_buffer_ref(hw_device_ctx_.get());

  // VAAPI needs an explicit frames pool: the encoder expects AV_PIX_FMT_VAAPI surfaces that were
  // uploaded from NV12/YUV420P system memory.
  if (type == AV_HWDEVICE_TYPE_VAAPI) {
    AVBufferRef* frames = av_hwframe_ctx_alloc(hw_device_ctx_.get());
    if (frames == nullptr) {
      return from_av(AVERROR(ENOMEM), "av_hwframe_ctx_alloc");
    }
    auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = context_->width;
    frames_ctx->height = context_->height;
    if (const int ret = av_hwframe_ctx_init(frames); ret < 0) {
      av_buffer_unref(&frames);
      return from_av(ret, "av_hwframe_ctx_init");
    }
    hw_frames_ctx_.reset(frames);
    context_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_.get());
    context_->pix_fmt = AV_PIX_FMT_VAAPI;
  }
  return ok();
}

Status Encoder::open_audio(const StreamInfo& input, const EncodeOptions& options) {
  const AVCodec* codec = avcodec_find_encoder_by_name(options.codec.c_str());
  if (codec == nullptr) {
    return fail(Code::unsupported, "encoder '" + options.codec + "' is not available");
  }

  context_.reset(avcodec_alloc_context3(codec));
  if (!context_) {
    return from_av(AVERROR(ENOMEM), "cannot allocate encoder context");
  }
  AVCodecContext* ctx = context_.get();

  sample_rate_ = options.sample_rate > 0 ? options.sample_rate
                                         : (input.params ? input.params->sample_rate : 0);
  if (sample_rate_ <= 0) {
    return fail(Code::invalid_argument, "audio encoder has no sample rate");
  }
  ctx->sample_rate = sample_rate_;
  ctx->time_base = AVRational{1, sample_rate_};

  if (options.channels > 0) {
    av_channel_layout_default(&ctx->ch_layout, options.channels);
  } else if (input.params && input.params->ch_layout.nb_channels > 0) {
    av_channel_layout_copy(&ctx->ch_layout, &input.params->ch_layout);
  } else {
    av_channel_layout_default(&ctx->ch_layout, 2);
  }
  channels_ = ctx->ch_layout.nb_channels;

  if (!options.sample_format.empty()) {
    sample_format_ = av_get_sample_fmt(options.sample_format.c_str());
    if (sample_format_ == AV_SAMPLE_FMT_NONE) {
      return fail(Code::invalid_argument,
                  "unknown sample format '" + options.sample_format + "'");
    }
  } else if (codec->sample_fmts != nullptr) {
    sample_format_ = codec->sample_fmts[0];
  } else {
    sample_format_ = AV_SAMPLE_FMT_FLTP;
  }
  ctx->sample_fmt = sample_format_;
  ctx->bit_rate = options.bit_rate;
  if (options.threads > 0) {
    ctx->thread_count = options.threads;
  }
  source_time_base_ = input.time_base;

  if (const int ret = avcodec_open2(ctx, codec, nullptr); ret < 0) {
    return from_av(ret, std::string("cannot open encoder ") + codec->name);
  }
  frame_size_ = ctx->frame_size > 0 ? ctx->frame_size : 1024;

  out_info_.type = AVMEDIA_TYPE_AUDIO;
  out_info_.stream_index = 0;
  out_info_.time_base = ctx->time_base;
  return ok();
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

Status Encoder::send_frame(const AVFrame* frame) {
  for (;;) {
    const int ret = avcodec_send_frame(context_.get(), frame);
    if (ret == 0) {
      if (frame != nullptr) {
        ++frames_in_;
      }
      return drain_locked("avcodec_receive_packet");
    }
    if (ret == AVERROR(EAGAIN)) {
      // Encoder is full: take what it has, then offer the frame again.
      if (auto drained = drain_locked("avcodec_receive_packet"); drained) {
        return drained;
      }
      continue;
    }
    return from_av(ret, "avcodec_send_frame");
  }
}

Status Encoder::push(const Frame& frame) {
  if (flushed_) {
    return fail(Code::invalid_argument, "Encoder::push: encoder already flushed");
  }
  if (frame.empty()) {
    return fail(Code::invalid_argument, "Encoder::push: empty frame");
  }

  // Decoded frames carry pts in the *input* stream time base; the encoder counts in its own
  // (1/fps or 1/sample_rate). Without this the output timeline is stretched by tb_in / tb_out.
  Frame rescaled;
  const AVFrame* source = frame.get();
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
      rescaled->pkt_dts = av_rescale_q(rescaled->pkt_dts, source_time_base_, context_->time_base);
    }
    source = rescaled.get();
  }

  // Hardware encoders take device surfaces, not system memory: upload before sending.
  Frame hw_frame;
  if (hw_frames_ctx_) {
    if (auto created = Frame::create(); created) {
      hw_frame = created.take();
    } else {
      return created.error();
    }
    if (const int ret = av_hwframe_get_buffer(hw_frames_ctx_.get(), hw_frame.get(), 0); ret < 0) {
      return from_av(ret, "av_hwframe_get_buffer");
    }
    if (const int ret = av_hwframe_transfer_data(hw_frame.get(), source, 0); ret < 0) {
      return from_av(ret, "av_hwframe_transfer_data (upload)");
    }
    hw_frame->pts = source->pts;
    source = hw_frame.get();
  }

  return send_frame(source);
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
  // `drain_locked` reports success as Code::ok, which must NOT be turned into an Expected
  // holding an error: an Error with code ok has no value, so the caller would see "no packet"
  // with error() == ok and report an empty failure message.
  if (auto drained = drain_locked("avcodec_receive_packet"); drained) {
    return drained;
  }
  if (!ready_.empty()) {
    Packet packet = std::move(ready_.front());
    ready_.pop_front();
    return packet;
  }
  if (finished_) {
    return fail(Code::end_of_stream, "encoder finished");
  }
  return fail(Code::need_more, "encoder has no packet yet");
}

}  // namespace avpipe
