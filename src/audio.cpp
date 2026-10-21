// SPDX-License-Identifier: MIT
#include <avpipe/audio.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace avpipe {

namespace {

constexpr double kToneFrequency = 440.0;
constexpr double kToneAmplitude = 0.25;

/// Channel layout with `channels` channels; only the counts this project actually uses matter,
/// so a small switch keeps libav's channel-layout API out of the rest of the code.
AVChannelLayout layout_for(int channels) {
  AVChannelLayout layout{};
  if (channels == 1) {
    av_channel_layout_default(&layout, 1);
  } else if (channels == 2) {
    av_channel_layout_default(&layout, 2);
  } else {
    av_channel_layout_default(&layout, channels > 0 ? channels : 2);
  }
  return layout;
}

}  // namespace

// ---------------------------------------------------------------------------
// ToneSource
// ---------------------------------------------------------------------------

ToneSource::ToneSource(int sample_rate, int channels, AVSampleFormat format, int samples_per_frame,
                       int total_frames)
    : sample_rate_(sample_rate),
      channels_(channels),
      format_(format),
      samples_per_frame_(samples_per_frame),
      total_frames_(total_frames) {}

Expected<Frame> ToneSource::pull() {
  if (produced_ >= total_frames_) {
    return fail(Code::end_of_stream, "tone source finished");
  }

  Frame frame;
  if (auto created = Frame::create(); created) {
    frame = created.take();
  } else {
    return created.error();
  }

  frame->format = static_cast<int>(format_);
  frame->sample_rate = sample_rate_;
  frame->nb_samples = samples_per_frame_;
  AVChannelLayout layout = layout_for(channels_);
  av_channel_layout_copy(&frame->ch_layout, &layout);
  av_channel_layout_uninit(&layout);
  frame->pts = samples_emitted_;
  if (const int ret = av_frame_get_buffer(frame.get(), 0); ret < 0) {
    return from_av(ret, "av_frame_get_buffer (tone)");
  }

  const bool planar = av_sample_fmt_is_planar(format_) != 0;
  const int bytes = av_get_bytes_per_sample(format_);
  for (int i = 0; i < samples_per_frame_; ++i) {
    const int64_t index = samples_emitted_ + i;
    const double phase = 2.0 * M_PI * kToneFrequency * static_cast<double>(index) /
                         static_cast<double>(sample_rate_);
    const double value = kToneAmplitude * std::sin(phase);
    for (int ch = 0; ch < channels_; ++ch) {
      std::uint8_t* target = planar
                                 ? frame->extended_data[ch] + static_cast<std::ptrdiff_t>(i) * bytes
                                 : frame->data[0] +
                                       static_cast<std::ptrdiff_t>(i * channels_ + ch) * bytes;
      switch (format_) {
        case AV_SAMPLE_FMT_FLTP:
        case AV_SAMPLE_FMT_FLT:
          *reinterpret_cast<float*>(target) = static_cast<float>(value);
          break;
        case AV_SAMPLE_FMT_S16P:
        case AV_SAMPLE_FMT_S16:
          *reinterpret_cast<std::int16_t*>(target) = static_cast<std::int16_t>(value * 32767.0);
          break;
        case AV_SAMPLE_FMT_S32P:
        case AV_SAMPLE_FMT_S32:
          *reinterpret_cast<std::int32_t*>(target) =
              static_cast<std::int32_t>(value * 2147483647.0);
          break;
        default:
          return fail(Code::unsupported,
                      std::string("tone source: unsupported sample format ") +
                          sample_format_name(static_cast<int>(format_)));
      }
    }
  }

  samples_emitted_ += samples_per_frame_;
  ++produced_;
  return frame;
}

// ---------------------------------------------------------------------------
// AudioResampler
// ---------------------------------------------------------------------------

Expected<std::unique_ptr<AudioResampler>> AudioResampler::open(FrameSource& source, int sample_rate,
                                                              int channels, AVSampleFormat format) {
  if (sample_rate <= 0) {
    return fail(Code::invalid_argument, "AudioResampler::open: sample rate must be positive");
  }
  std::unique_ptr<AudioResampler> resampler(new AudioResampler());
  resampler->source_ = &source;
  resampler->out_sample_rate_ = sample_rate;
  resampler->out_channels_ = channels > 0 ? channels : 2;
  resampler->out_format_ = format;
  resampler->out_layout_ = layout_for(resampler->out_channels_);
  return resampler;
}

AudioResampler::~AudioResampler() = default;

Status AudioResampler::configure(const AVFrame& first) {
  const int in_rate = first.sample_rate;
  if (in_rate <= 0) {
    return fail(Code::invalid_argument, "resampler: input frame has no sample rate");
  }

  SwrContext* raw = nullptr;
  const int ret = swr_alloc_set_opts2(&raw, &out_layout_, out_format_, out_sample_rate_,
                                      &first.ch_layout, static_cast<AVSampleFormat>(first.format),
                                      in_rate, 0, nullptr);
  if (ret < 0 || raw == nullptr) {
    return from_av(ret < 0 ? ret : AVERROR(ENOMEM), "swr_alloc_set_opts2");
  }
  swr_.reset(raw);
  if (const int init = swr_init(swr_.get()); init < 0) {
    return from_av(init, "swr_init");
  }
  configured_ = true;
  return ok();
}

Expected<Frame> AudioResampler::convert(const AVFrame& input) {
  const int in_rate = input.sample_rate;
  const int out_samples = static_cast<int>(av_rescale_rnd(
      swr_get_delay(swr_.get(), in_rate) + input.nb_samples, out_sample_rate_, in_rate,
      AV_ROUND_UP));

  Frame out;
  if (auto created = Frame::create(); created) {
    out = created.take();
  } else {
    return created.error();
  }
  out->format = static_cast<int>(out_format_);
  out->sample_rate = out_sample_rate_;
  out->nb_samples = out_samples;
  av_channel_layout_copy(&out->ch_layout, &out_layout_);
  out->pts = static_cast<int64_t>(av_rescale_q(input.pts, AVRational{1, in_rate},
                                               AVRational{1, out_sample_rate_}));
  if (const int ret = av_frame_get_buffer(out.get(), 0); ret < 0) {
    return from_av(ret, "av_frame_get_buffer (resampled)");
  }

  const int converted = swr_convert(swr_.get(), out->data, out_samples,
                                    const_cast<const uint8_t**>(input.data), input.nb_samples);
  if (converted < 0) {
    return from_av(converted, "swr_convert");
  }
  out->nb_samples = converted;
  ++converted_;
  return out;
}

Expected<Frame> AudioResampler::drain() {
  if (drained_) {
    return fail(Code::end_of_stream, "resampler drained");
  }
  drained_ = true;

  const int out_samples = static_cast<int>(
      av_rescale_rnd(swr_get_delay(swr_.get(), out_sample_rate_), out_sample_rate_,
                     out_sample_rate_, AV_ROUND_UP));
  if (out_samples <= 0) {
    return fail(Code::end_of_stream, "resampler has no delayed samples");
  }

  Frame out;
  if (auto created = Frame::create(); created) {
    out = created.take();
  } else {
    return created.error();
  }
  out->format = static_cast<int>(out_format_);
  out->sample_rate = out_sample_rate_;
  out->nb_samples = out_samples;
  av_channel_layout_copy(&out->ch_layout, &out_layout_);
  if (const int ret = av_frame_get_buffer(out.get(), 0); ret < 0) {
    return from_av(ret, "av_frame_get_buffer (resampler drain)");
  }
  const int converted = swr_convert(swr_.get(), out->data, out_samples, nullptr, 0);
  if (converted < 0) {
    return from_av(converted, "swr_convert (drain)");
  }
  if (converted == 0) {
    return fail(Code::end_of_stream, "resampler drained");
  }
  out->nb_samples = converted;
  return out;
}

Expected<Frame> AudioResampler::pull() {
  for (;;) {
    if (!configured_) {
      auto first = source_->pull();
      if (!first) {
        return first.error();
      }
      if (auto status = configure(*first.value().get()); status) {
        return status;
      }
      pending_ = first.take();
    }

    if (pending_) {
      Frame frame = std::move(pending_);
      pending_.reset();
      // Pass-through when the frame already matches the requested output format.
      if (frame->sample_rate == out_sample_rate_ &&
          frame->format == static_cast<int>(out_format_) &&
          frame->ch_layout.nb_channels == out_channels_) {
        return frame;
      }
      return convert(*frame.get());
    }

    auto next = source_->pull();
    if (!next) {
      if (next.error().code == Code::end_of_stream) {
        return drain();
      }
      return next.error();
    }
    pending_ = next.take();
  }
}

// ---------------------------------------------------------------------------
// AudioSlicer
// ---------------------------------------------------------------------------

Expected<std::unique_ptr<AudioSlicer>> AudioSlicer::open(FrameSource& source, int frame_size) {
  if (frame_size <= 0) {
    return fail(Code::invalid_argument, "AudioSlicer::open: frame size must be positive");
  }
  std::unique_ptr<AudioSlicer> slicer(new AudioSlicer());
  slicer->source_ = &source;
  slicer->frame_size_ = frame_size;
  return slicer;
}

AudioSlicer::~AudioSlicer() = default;

Expected<Frame> AudioSlicer::pull() {
  // First call: learn the format from one frame so the FIFO can be allocated.
  if (!fifo_) {
    auto first = source_->pull();
    if (!first) {
      return first.error();
    }
    const AVFrame* probe = first.value().get();
    format_ = static_cast<AVSampleFormat>(probe->format);
    channels_ = probe->ch_layout.nb_channels;
    fifo_.reset(av_audio_fifo_alloc(format_, channels_, frame_size_));
    if (!fifo_) {
      return from_av(AVERROR(ENOMEM), "av_audio_fifo_alloc");
    }
    if (const int ret =
            av_audio_fifo_write(fifo_.get(), reinterpret_cast<void**>(probe->extended_data),
                                probe->nb_samples);
        ret < 0) {
      return from_av(ret, "av_audio_fifo_write");
    }
  }

  for (;;) {
    const int available = av_audio_fifo_size(fifo_.get());
    if (available >= frame_size_) {
      Frame out;
      if (auto created = Frame::create(); created) {
        out = created.take();
      } else {
        return created.error();
      }
      out->format = static_cast<int>(format_);
      out->nb_samples = frame_size_;
      AVChannelLayout layout{};
      av_channel_layout_default(&layout, channels_);
      av_channel_layout_copy(&out->ch_layout, &layout);
      if (const int ret = av_frame_get_buffer(out.get(), 0); ret < 0) {
        return from_av(ret, "av_frame_get_buffer (sliced)");
      }
      if (const int read =
              av_audio_fifo_read(fifo_.get(), reinterpret_cast<void**>(out->extended_data),
                                 frame_size_);
          read < 0) {
        return from_av(read, "av_audio_fifo_read");
      }
      out->pts = frames_emitted_ * static_cast<int64_t>(frame_size_);
      ++frames_emitted_;
      return out;
    }

    if (upstream_eof_) {
      // Pad the tail with silence: encoders reject a short final frame.
      if (available > 0) {
        Frame out;
        if (auto created = Frame::create(); created) {
          out = created.take();
        } else {
          return created.error();
        }
        out->format = static_cast<int>(format_);
        out->nb_samples = frame_size_;
        AVChannelLayout layout{};
        av_channel_layout_default(&layout, channels_);
        av_channel_layout_copy(&out->ch_layout, &layout);
        if (const int ret = av_frame_get_buffer(out.get(), 0); ret < 0) {
          return from_av(ret, "av_frame_get_buffer (padded tail)");
        }
        if (const int read =
                av_audio_fifo_read(fifo_.get(), reinterpret_cast<void**>(out->extended_data),
                                   available);
            read < 0) {
          return from_av(read, "av_audio_fifo_read (tail)");
        }
        // Zero the remainder so the padded samples are silence, not uninitialised memory.
        const int bytes_per_sample = av_get_bytes_per_sample(format_);
        for (int ch = 0; ch < channels_; ++ch) {
          std::memset(out->extended_data[ch] + static_cast<std::ptrdiff_t>(available) *
                                               bytes_per_sample,
                      0, static_cast<std::size_t>(frame_size_ - available) * bytes_per_sample);
        }
        out->pts = frames_emitted_ * static_cast<int64_t>(frame_size_);
        ++frames_emitted_;
        finished_ = true;
        return out;
      }
      finished_ = true;
      return fail(Code::end_of_stream, "slicer finished");
    }

    auto next = source_->pull();
    if (!next) {
      if (next.error().code == Code::end_of_stream) {
        upstream_eof_ = true;
        continue;
      }
      return next.error();
    }
    const AVFrame* frame = next.value().get();
    if (const int written =
            av_audio_fifo_write(fifo_.get(), reinterpret_cast<void**>(frame->extended_data),
                                frame->nb_samples);
        written < 0) {
      return from_av(written, "av_audio_fifo_write");
    }
  }
}

}  // namespace avpipe
