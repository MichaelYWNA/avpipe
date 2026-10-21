// SPDX-License-Identifier: MIT
#include <avpipe/pipeline.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <avpipe/audio.hpp>
#include <avpipe/decode.hpp>
#include <avpipe/demux.hpp>
#include <avpipe/filter.hpp>
#include <avpipe/mux.hpp>
#include <avpipe/scale.hpp>
#include <avpipe/stage.hpp>

namespace avpipe {

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Copies stream parameters and overrides the frame size (used when a scaler changes geometry
/// before the encoder).
Expected<StreamInfo> resized_video_info(const StreamInfo& base, int width, int height) {
  if (!base.params) {
    return fail(Code::invalid_argument, "resized_video_info: source stream has no parameters");
  }
  StreamInfo out;
  out.type = base.type;
  out.stream_index = base.stream_index;
  out.time_base = base.time_base;
  out.params.reset(avcodec_parameters_alloc());
  if (!out.params) {
    return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
  }
  if (const int ret = avcodec_parameters_copy(out.params.get(), base.params.get()); ret < 0) {
    return from_av(ret, "cannot copy codec parameters");
  }
  if (width > 0 && height > 0) {
    out.params->width = width;
    out.params->height = height;
  }
  return out;
}

/// Copies audio parameters with optional rate/channel overrides.
Expected<StreamInfo> audio_info(const StreamInfo& base, int sample_rate, int channels) {
  if (!base.params) {
    return fail(Code::invalid_argument, "audio_info: source stream has no parameters");
  }
  StreamInfo out;
  out.type = base.type;
  out.stream_index = base.stream_index;
  out.time_base = base.time_base;
  out.params.reset(avcodec_parameters_alloc());
  if (!out.params) {
    return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
  }
  if (const int ret = avcodec_parameters_copy(out.params.get(), base.params.get()); ret < 0) {
    return from_av(ret, "cannot copy codec parameters");
  }
  if (sample_rate > 0) {
    out.params->sample_rate = sample_rate;
    out.time_base = AVRational{1, sample_rate};
  }
  if (channels > 0) {
    av_channel_layout_default(&out.params->ch_layout, channels);
  }
  return out;
}

/// Pulls every packet the encoder currently has and writes it out.
Status drain_encoder(Encoder& encoder, Muxer& muxer, PipelineStats& stats) {
  for (;;) {
    auto packet = encoder.pull();
    if (!packet) {
      if (packet.error().code == Code::need_more || packet.error().code == Code::end_of_stream) {
        return ok();
      }
      return packet.error();
    }
    if (auto written = muxer.write(packet.value()); written) {
      return written;
    }
    ++stats.packets_written;
  }
}

/// Deterministic synthetic video: a luma ramp with a moving bright block, chroma flat at 128.
class SyntheticSource : public FrameSource {
 public:
  SyntheticSource(int width, int height, int fps, int frames)
      : width_(width), height_(height), fps_(fps), total_(frames) {}

  Expected<Frame> pull() override {
    if (produced_ >= total_) {
      return fail(Code::end_of_stream, "synthetic source finished");
    }

    Frame frame;
    if (auto created = Frame::create(); created) {
      frame = created.take();
    } else {
      return created.error();
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width_;
    frame->height = height_;
    frame->pts = produced_;
    if (const int ret = av_frame_get_buffer(frame.get(), 0); ret < 0) {
      return from_av(ret, "av_frame_get_buffer (synthetic)");
    }

    draw(frame.get(), produced_);
    ++produced_;
    return frame;
  }

 private:
  void draw(AVFrame* frame, int index) {
    const int box_x = (index * 4) % (width_ > 64 ? width_ - 32 : 1);
    for (int y = 0; y < height_; ++y) {
      std::uint8_t* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
      for (int x = 0; x < width_; ++x) {
        row[x] = static_cast<std::uint8_t>((x * 255) / width_);
      }
    }
    for (int y = 16; y < 48 && y < height_; ++y) {
      std::uint8_t* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
      for (int x = box_x; x < box_x + 32 && x < width_; ++x) {
        row[x] = 235;
      }
    }
    for (int y = 0; y < height_ / 2; ++y) {
      std::memset(frame->data[1] + static_cast<std::ptrdiff_t>(y) * frame->linesize[1], 128,
                  static_cast<std::size_t>(width_ / 2));
      std::memset(frame->data[2] + static_cast<std::ptrdiff_t>(y) * frame->linesize[2], 128,
                  static_cast<std::size_t>(width_ / 2));
    }
  }

  int width_ = 0;
  int height_ = 0;
  int fps_ = 0;
  int total_ = 0;
  int produced_ = 0;
};

}  // namespace

std::string PipelineStats::summary() const {
  std::ostringstream os;
  os << "packets read=" << packets_read << " frames decoded=" << frames_decoded
     << " frames scaled=" << frames_converted << " frames encoded=" << frames_encoded
     << " packets written=" << packets_written << " wall=" << wall_ms << " ms";
  return os.str();
}

Expected<PipelineStats> transcode(const TranscodeRequest& request) {
  PipelineStats stats;
  const auto started = Clock::now();

  auto demuxer = Demuxer::open(request.input);
  if (!demuxer) {
    return demuxer.error();
  }
  if (auto selected = demuxer.value()->select_stream(AVMEDIA_TYPE_VIDEO); selected) {
    return selected;
  }
  const StreamInfo* source_info = demuxer.value()->find(AVMEDIA_TYPE_VIDEO);
  if (source_info == nullptr) {
    return fail(Code::unsupported, "no video stream in '" + request.input + "'");
  }

  auto decoder = Decoder::open(*demuxer.value(), *source_info);
  if (!decoder) {
    return decoder.error();
  }

  FrameSource* frames = decoder.value().get();
  std::unique_ptr<FilterSource> filter;
  std::unique_ptr<ScalingSource> scaler;
  Frame first_frame;  // buffered from the filter so the encoder can be sized from real output

  if (!request.filter.empty()) {
    auto opened = FilterSource::open(*frames, request.filter, source_info->time_base);
    if (!opened) {
      return opened.error();
    }
    filter = opened.take();
    frames = filter.get();
    // The filter decides the output geometry (crop/scale/pad) and the output time base (fps).
    // Both must be measured from a real frame before the encoder is opened.
    auto probe_frame = frames->pull();
    if (!probe_frame) {
      if (probe_frame.error().code != Code::end_of_stream) {
        return probe_frame.error();
      }
    } else {
      first_frame = probe_frame.take();
    }
  }
  if (request.width > 0 && request.height > 0) {
    auto opened = ScalingSource::open(*frames, request.width, request.height);
    if (!opened) {
      return opened.error();
    }
    scaler = opened.take();
    frames = scaler.get();
  }

  auto encoder_input = resized_video_info(*source_info, request.width, request.height);
  if (!encoder_input) {
    return encoder_input.error();
  }
  if (filter) {
    const AVRational filter_tb = filter->output_time_base();
    encoder_input.value().time_base = filter_tb;
    if (!first_frame.empty() && scaler == nullptr) {
      encoder_input.value().params->width = first_frame->width;
      encoder_input.value().params->height = first_frame->height;
    }
  }

  // Frame rate: default to the source cadence. An explicit but different rate is only allowed
  // when the filter chain converts it; otherwise the output metadata would lie.
  EncodeOptions encode_options = request.encode;
  const double source_fps =
      source_info->params->framerate.num > 0 ? av_q2d(source_info->params->framerate) : 0.0;
  const bool filter_sets_fps = request.filter.find("fps=") != std::string::npos;
  if (request.force_fps && !filter_sets_fps && source_fps > 0.0 &&
      std::abs(static_cast<double>(encode_options.fps) - source_fps) > 0.5) {
    return fail(Code::unsupported,
                "frame rate conversion (" + std::to_string(encode_options.fps) + " fps from " +
                    std::to_string(source_fps) +
                    " fps source) needs a filter; pass --filter 'fps=N' or drop --fps");
  }
  if (!request.force_fps && source_fps > 0.0) {
    encode_options.fps = static_cast<int>(source_fps + 0.5);
  }

  auto encoder = Encoder::open(encoder_input.value(), encode_options);
  if (!encoder) {
    return encoder.error();
  }

  auto muxer = Muxer::open(request.output, encoder.value()->out_info());
  if (!muxer) {
    return muxer.error();
  }

  if (!first_frame.empty()) {
    if (auto pushed = encoder.value()->push(first_frame); pushed) {
      return pushed;
    }
    if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
      return written;
    }
  }

  for (;;) {
    auto frame = frames->pull();
    if (!frame) {
      if (frame.error().code == Code::end_of_stream) {
        break;
      }
      return frame.error();
    }
    if (auto pushed = encoder.value()->push(frame.value()); pushed) {
      return pushed;
    }
    if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
      return written;
    }
  }

  if (auto flushed = encoder.value()->flush(); flushed) {
    return flushed;
  }
  if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
    return written;
  }
  if (auto closed = muxer.value()->close(); closed) {
    return closed;
  }

  stats.packets_read = demuxer.value()->packets_read();
  stats.frames_decoded = decoder.value()->frames_decoded();
  stats.frames_converted = scaler ? scaler->frames_converted() : 0;
  stats.frames_encoded = encoder.value()->frames_in();
  stats.wall_ms = elapsed_ms(started);
  return stats;
}

Expected<PipelineStats> transcode_audio(const AudioTranscodeRequest& request) {
  PipelineStats stats;
  const auto started = Clock::now();

  auto demuxer = Demuxer::open(request.input);
  if (!demuxer) {
    return demuxer.error();
  }
  if (auto selected = demuxer.value()->select_stream(AVMEDIA_TYPE_AUDIO); selected) {
    return selected;
  }
  const StreamInfo* source_info = demuxer.value()->find(AVMEDIA_TYPE_AUDIO);
  if (source_info == nullptr) {
    return fail(Code::unsupported, "no audio stream in '" + request.input + "'");
  }

  auto decoder = Decoder::open(*demuxer.value(), *source_info);
  if (!decoder) {
    return decoder.error();
  }

  auto encoder_input = audio_info(*source_info, request.sample_rate, request.channels);
  if (!encoder_input) {
    return encoder_input.error();
  }
  auto encoder = Encoder::open(encoder_input.value(), request.encode);
  if (!encoder) {
    return encoder.error();
  }

  // The encoder decides rate/layout/sample format; the resampler has to match it exactly and the
  // slicer has to feed exactly frame_size samples per push (AAC: 1024).
  auto resampler =
      AudioResampler::open(*decoder.value(), encoder.value()->audio_sample_rate(),
                           encoder.value()->audio_channels(),
                           encoder.value()->audio_sample_format());
  if (!resampler) {
    return resampler.error();
  }

  auto slicer = AudioSlicer::open(*resampler.value(), encoder.value()->audio_frame_size());
  if (!slicer) {
    return slicer.error();
  }

  auto muxer = Muxer::open(request.output, encoder.value()->out_info());
  if (!muxer) {
    return muxer.error();
  }

  for (;;) {
    auto frame = slicer.value()->pull();
    if (!frame) {
      if (frame.error().code == Code::end_of_stream) {
        break;
      }
      return frame.error();
    }
    if (auto pushed = encoder.value()->push(frame.value()); pushed) {
      return pushed;
    }
    if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
      return written;
    }
  }

  if (auto flushed = encoder.value()->flush(); flushed) {
    return flushed;
  }
  if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
    return written;
  }
  if (auto closed = muxer.value()->close(); closed) {
    return closed;
  }

  stats.packets_read = demuxer.value()->packets_read();
  stats.frames_decoded = decoder.value()->frames_decoded();
  stats.frames_converted = resampler.value()->frames_converted();
  stats.frames_encoded = encoder.value()->frames_in();
  stats.wall_ms = elapsed_ms(started);
  return stats;
}

Expected<PipelineStats> generate(const GenerateRequest& request) {
  PipelineStats stats;
  const auto started = Clock::now();

  if (request.audio) {
    StreamInfo template_info;
    template_info.type = AVMEDIA_TYPE_AUDIO;
    template_info.time_base = AVRational{1, request.sample_rate};
    template_info.params.reset(avcodec_parameters_alloc());
    if (!template_info.params) {
      return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
    }
    template_info.params->codec_type = AVMEDIA_TYPE_AUDIO;
    template_info.params->sample_rate = request.sample_rate;
    av_channel_layout_default(&template_info.params->ch_layout, request.channels);

    auto encoder = Encoder::open(template_info, request.encode);
    if (!encoder) {
      return encoder.error();
    }
    auto muxer = Muxer::open(request.output, encoder.value()->out_info());
    if (!muxer) {
      return muxer.error();
    }

    const int frame_size = encoder.value()->audio_frame_size();
    const int rate = encoder.value()->audio_sample_rate();
    const int channels = encoder.value()->audio_channels();
    const int total_frames =
        static_cast<int>(std::ceil(request.seconds * rate / static_cast<double>(frame_size)));
    if (total_frames <= 0) {
      return fail(Code::invalid_argument, "generate: duration must be positive");
    }

    ToneSource source(rate, channels, encoder.value()->audio_sample_format(), frame_size,
                      total_frames);
    for (;;) {
      auto frame = source.pull();
      if (!frame) {
        if (frame.error().code == Code::end_of_stream) {
          break;
        }
        return frame.error();
      }
      ++stats.frames_decoded;
      if (auto pushed = encoder.value()->push(frame.value()); pushed) {
        return pushed;
      }
      if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
        return written;
      }
    }

    if (auto flushed = encoder.value()->flush(); flushed) {
      return flushed;
    }
    if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
      return written;
    }
    if (auto closed = muxer.value()->close(); closed) {
      return closed;
    }
    stats.frames_encoded = encoder.value()->frames_in();
    stats.wall_ms = elapsed_ms(started);
    return stats;
  }

  if (request.frames <= 0) {
    return fail(Code::invalid_argument, "generate: frame count must be positive");
  }

  StreamInfo template_info;
  template_info.type = AVMEDIA_TYPE_VIDEO;
  template_info.stream_index = 0;
  template_info.time_base = AVRational{1, request.fps};
  template_info.params.reset(avcodec_parameters_alloc());
  if (!template_info.params) {
    return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
  }
  template_info.params->codec_type = AVMEDIA_TYPE_VIDEO;
  template_info.params->width = request.width;
  template_info.params->height = request.height;
  template_info.params->framerate = AVRational{request.fps, 1};

  auto encoder = Encoder::open(template_info, request.encode);
  if (!encoder) {
    return encoder.error();
  }
  auto muxer = Muxer::open(request.output, encoder.value()->out_info());
  if (!muxer) {
    return muxer.error();
  }

  SyntheticSource source(request.width, request.height, request.fps, request.frames);
  for (;;) {
    auto frame = source.pull();
    if (!frame) {
      if (frame.error().code == Code::end_of_stream) {
        break;
      }
      return frame.error();
    }
    ++stats.frames_decoded;  // produced
    if (auto pushed = encoder.value()->push(frame.value()); pushed) {
      return pushed;
    }
    if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
      return written;
    }
  }

  if (auto flushed = encoder.value()->flush(); flushed) {
    return flushed;
  }
  if (auto written = drain_encoder(*encoder.value(), *muxer.value(), stats); written) {
    return written;
  }
  if (auto closed = muxer.value()->close(); closed) {
    return closed;
  }

  stats.frames_encoded = encoder.value()->frames_in();
  stats.wall_ms = elapsed_ms(started);
  return stats;
}

Expected<ProbeReport> probe(const std::string& path) {
  auto demuxer = Demuxer::open(path);
  if (!demuxer) {
    return demuxer.error();
  }

  ProbeReport report;
  report.format = demuxer.value()->format_name();
  report.duration_s = demuxer.value()->duration_seconds();

  std::vector<int64_t> per_stream(demuxer.value()->streams().size(), 0);
  for (;;) {
    auto packet = demuxer.value()->pull();
    if (!packet) {
      if (packet.error().code == Code::end_of_stream) {
        break;
      }
      return packet.error();
    }
    ++report.packets;
    const int index = packet.value()->stream_index;
    if (index >= 0 && static_cast<std::size_t>(index) < per_stream.size()) {
      ++per_stream[static_cast<std::size_t>(index)];
    }
  }

  std::ostringstream head;
  head << "format=" << report.format << " duration=" << report.duration_s << "s"
       << " packets=" << report.packets;
  report.lines.push_back(head.str());

  for (const StreamInfo& info : demuxer.value()->streams()) {
    std::ostringstream line;
    line << "  " << info.describe();
    if (info.stream_index >= 0 &&
        static_cast<std::size_t>(info.stream_index) < per_stream.size()) {
      line << " packets=" << per_stream[static_cast<std::size_t>(info.stream_index)];
    }
    report.lines.push_back(line.str());
  }

  std::error_code ec;
  report.size_bytes = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec) {
    report.size_bytes = 0;
  }
  return report;
}

Expected<BenchReport> bench(const TranscodeRequest& request, int runs) {
  if (runs <= 0) {
    return fail(Code::invalid_argument, "bench: run count must be positive");
  }

  BenchReport report;
  report.runs = runs;
  PipelineStats last;
  for (int i = 0; i < runs; ++i) {
    auto stats = transcode(request);
    if (!stats) {
      return stats.error();
    }
    last = stats.value();
    report.total_ms += last.wall_ms;
  }
  report.frames_per_run = last.frames_encoded;
  if (report.total_ms > 0.0) {
    report.frames_per_second =
        (static_cast<double>(report.frames_per_run) * report.runs) / (report.total_ms / 1000.0);
  }
  return report;
}

}  // namespace avpipe
