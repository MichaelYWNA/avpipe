// SPDX-License-Identifier: MIT
#include <erde/pipeline.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <utility>

#include <erde/audio.hpp>
#include <erde/decode.hpp>
#include <erde/demux.hpp>
#include <erde/filter.hpp>
#include <erde/mux.hpp>
#include <erde/scale.hpp>
#include <erde/stage.hpp>

namespace erde {

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Copies stream parameters and overrides the frame size (used when a scaler changes geometry).
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

/// One encoded stream feeding the muxer. Video and audio use the same shape, which is what makes
/// the A/V loop below a two-branch alternation instead of two copies of the same code.
struct Chain {
  std::unique_ptr<Decoder> decoder;
  std::unique_ptr<FilterSource> filter;   ///< optional libavfilter stage
  std::unique_ptr<ScalingSource> scaler;  ///< video only
  std::unique_ptr<AudioResampler> resampler;
  std::unique_ptr<AudioSlicer> slicer;
  FrameSource* frames = nullptr;
  std::unique_ptr<Encoder> encoder;
  int muxer_index = -1;
  bool done = false;
  double last_time = 0.0;  ///< seconds, used to decide which chain to pull next
  int64_t decoded = 0;
};

Status write_ready_packets(Chain& chain, Muxer& muxer, PipelineStats& stats, bool audio) {
  for (;;) {
    auto packet = chain.encoder->pull();
    if (!packet) {
      if (packet.error().code == Code::need_more || packet.error().code == Code::end_of_stream) {
        return ok();
      }
      return packet.error();
    }
    if (auto written = muxer.write(packet.value(), chain.muxer_index); written) {
      return written;
    }
    ++stats.packets_written;
    if (audio) {
      ++stats.audio_packets_written;
    }
  }
}

Status pump(Chain& chain, Muxer& muxer, PipelineStats& stats, bool audio) {
  auto frame = chain.frames->pull();
  if (!frame) {
    if (frame.error().code == Code::end_of_stream) {
      chain.done = true;
      return ok();
    }
    return frame.error();
  }

  const AVFrame* raw = frame.value().get();
  const AVRational tb =
      audio ? AVRational{1, raw->sample_rate} : chain.encoder->out_info().time_base;
  chain.last_time = raw->pts == AV_NOPTS_VALUE ? 0.0 : raw->pts * av_q2d(tb);
  ++chain.decoded;

  if (auto pushed = chain.encoder->push(frame.value()); pushed) {
    return pushed;
  }
  return write_ready_packets(chain, muxer, stats, audio);
}

Status flush_chain(Chain& chain, Muxer& muxer, PipelineStats& stats, bool audio) {
  if (!chain.encoder) {
    return ok();
  }
  if (auto flushed = chain.encoder->flush(); flushed) {
    return flushed;
  }
  return write_ready_packets(chain, muxer, stats, audio);
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

/// Builds the video branch: decoder -> [filter] -> [scale] -> encoder.
Status open_video_chain(Demuxer& demuxer, const StreamInfo& info, const TranscodeRequest& request,
                        Chain& chain, Frame& buffered_first) {
  auto decoder = Decoder::open(demuxer, info);
  if (!decoder) {
    return decoder.error();
  }
  chain.decoder = decoder.take();
  chain.frames = chain.decoder.get();

  if (!request.filter.empty()) {
    auto opened = FilterSource::open(*chain.frames, request.filter, info.time_base);
    if (!opened) {
      return opened.error();
    }
    chain.filter = opened.take();
    chain.frames = chain.filter.get();
    // The filter decides output geometry and time base; measure both from a real frame.
    auto probe_frame = chain.frames->pull();
    if (probe_frame) {
      buffered_first = probe_frame.take();
    } else if (probe_frame.error().code != Code::end_of_stream) {
      return probe_frame.error();
    }
  }

  if (request.width > 0 && request.height > 0) {
    auto opened = ScalingSource::open(*chain.frames, request.width, request.height);
    if (!opened) {
      return opened.error();
    }
    chain.scaler = opened.take();
    chain.frames = chain.scaler.get();
  }

  auto encoder_input = resized_video_info(info, request.width, request.height);
  if (!encoder_input) {
    return encoder_input.error();
  }
  if (chain.filter) {
    encoder_input.value().time_base = chain.filter->output_time_base();
    if (!buffered_first.empty() && chain.scaler == nullptr) {
      encoder_input.value().params->width = buffered_first->width;
      encoder_input.value().params->height = buffered_first->height;
    }
  }

  // Frame rate: default to the source cadence. An explicit but different rate is only allowed
  // when the filter chain converts it; otherwise the output metadata would lie.
  EncodeOptions encode_options = request.encode;
  const double source_fps = info.params->framerate.num > 0 ? av_q2d(info.params->framerate) : 0.0;
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
  chain.encoder = encoder.take();
  return ok();
}

/// Builds the audio branch: decoder -> [filter] -> resample -> slice -> encoder.
Status open_audio_chain(Demuxer& demuxer, const StreamInfo& info, const EncodeOptions& options,
                        const std::string& filter, Chain& chain) {
  auto decoder = Decoder::open(demuxer, info);
  if (!decoder) {
    return decoder.error();
  }
  chain.decoder = decoder.take();
  chain.frames = chain.decoder.get();

  if (!filter.empty()) {
    auto opened = FilterSource::open(*chain.frames, filter, info.time_base);
    if (!opened) {
      return opened.error();
    }
    chain.filter = opened.take();
    chain.frames = chain.filter.get();
  }

  auto encoder_input = audio_info(info, options.sample_rate, options.channels);
  if (!encoder_input) {
    return encoder_input.error();
  }
  auto encoder = Encoder::open(encoder_input.value(), options);
  if (!encoder) {
    return encoder.error();
  }
  chain.encoder = encoder.take();

  // The encoder decides rate/layout/sample format; the resampler has to match it exactly and the
  // slicer has to feed exactly frame_size samples per push (AAC: 1024).
  auto resampler = AudioResampler::open(*chain.frames, chain.encoder->audio_sample_rate(),
                                        chain.encoder->audio_channels(),
                                        chain.encoder->audio_sample_format());
  if (!resampler) {
    return resampler.error();
  }
  chain.resampler = resampler.take();
  chain.frames = chain.resampler.get();

  auto slicer = AudioSlicer::open(*chain.frames, chain.encoder->audio_frame_size());
  if (!slicer) {
    return slicer.error();
  }
  chain.slicer = slicer.take();
  chain.frames = chain.slicer.get();
  return ok();
}

}  // namespace

std::string PipelineStats::summary() const {
  std::ostringstream os;
  os << "packets read=" << packets_read << " frames decoded=" << frames_decoded;
  if (audio_streams > 0) {
    os << " audio frames=" << audio_frames_decoded;
  }
  os << " frames scaled=" << frames_converted << " frames encoded=" << frames_encoded;
  if (audio_frames_encoded > 0) {
    os << " audio frames encoded=" << audio_frames_encoded;
  }
  os << " packets written=" << packets_written;
  if (audio_streams > 0 && video_streams > 0) {
    os << " (video " << (packets_written - audio_packets_written) << " + audio "
       << audio_packets_written << ")";
  }
  os << " wall=" << wall_ms << " ms";
  return os.str();
}

Expected<PipelineStats> transcode(const TranscodeRequest& request) {
  PipelineStats stats;
  const auto started = Clock::now();

  auto probe_demuxer =
      Demuxer::open(request.input, request.input_format, request.input_options);
  if (!probe_demuxer) {
    return probe_demuxer.error();
  }
  const bool has_video = probe_demuxer.value()->find(AVMEDIA_TYPE_VIDEO) != nullptr;
  const bool has_audio = probe_demuxer.value()->find(AVMEDIA_TYPE_AUDIO) != nullptr;
  probe_demuxer.value().reset();

  const bool want_video = request.include_video && has_video;
  const bool want_audio = request.include_audio && has_audio;
  if (!want_video && !want_audio) {
    return fail(Code::unsupported, "no usable stream in '" + request.input + "'");
  }

  auto muxer = Muxer::open(request.output);
  if (!muxer) {
    return muxer.error();
  }

  Chain video;
  Chain audio;
  Frame buffered_first;

  // One demuxer per stream: a Demuxer hands out a single selected stream, and the two branches
  // run at different cadences, so sharing one reader would serialise them.
  std::unique_ptr<Demuxer> video_demuxer;
  std::unique_ptr<Demuxer> audio_demuxer;

  if (want_video) {
    auto opened =
        Demuxer::open(request.input, request.input_format, request.input_options);
    if (!opened) {
      return opened.error();
    }
    video_demuxer = opened.take();
    if (auto selected = video_demuxer->select_stream(AVMEDIA_TYPE_VIDEO); selected) {
      return selected;
    }
    const StreamInfo* info = video_demuxer->find(AVMEDIA_TYPE_VIDEO);
    if (auto status = open_video_chain(*video_demuxer, *info, request, video, buffered_first);
        status) {
      return status;
    }
  }

  if (want_audio) {
    auto opened =
        Demuxer::open(request.input, request.input_format, request.input_options);
    if (!opened) {
      return opened.error();
    }
    audio_demuxer = opened.take();
    if (auto selected = audio_demuxer->select_stream(AVMEDIA_TYPE_AUDIO); selected) {
      return selected;
    }
    const StreamInfo* info = audio_demuxer->find(AVMEDIA_TYPE_AUDIO);
    if (auto status =
            open_audio_chain(*audio_demuxer, *info, request.audio_encode, request.audio_filter,
                             audio);
        status) {
      return status;
    }
  }

  if (video.encoder) {
    auto index = muxer.value()->add_stream(video.encoder->out_info());
    if (!index) {
      return index.error();
    }
    video.muxer_index = index.value();
  }
  if (audio.encoder) {
    auto index = muxer.value()->add_stream(audio.encoder->out_info());
    if (!index) {
      return index.error();
    }
    audio.muxer_index = index.value();
  }
  if (auto started_ok = muxer.value()->start(); started_ok) {
    return started_ok;
  }

  // The frame buffered by a video filter has to be fed before the loop starts.
  if (video.encoder && !buffered_first.empty()) {
    if (auto pushed = video.encoder->push(buffered_first); pushed) {
      return pushed;
    }
    if (auto written = write_ready_packets(video, *muxer.value(), stats, false); written) {
      return written;
    }
  }

  // Pull whichever stream is behind on the timeline so the muxer sees roughly ordered packets;
  // av_interleaved_write_frame() does the actual interleaving.
  while (!video.done || !audio.done) {
    const bool have_video = video.encoder != nullptr && !video.done;
    const bool have_audio = audio.encoder != nullptr && !audio.done;
    const bool take_video = have_video && (!have_audio || video.last_time <= audio.last_time);
    if (take_video) {
      if (auto status = pump(video, *muxer.value(), stats, false); status) {
        return status;
      }
    } else if (have_audio) {
      if (auto status = pump(audio, *muxer.value(), stats, true); status) {
        return status;
      }
    } else {
      break;
    }
  }

  if (auto status = flush_chain(video, *muxer.value(), stats, false); status) {
    return status;
  }
  if (auto status = flush_chain(audio, *muxer.value(), stats, true); status) {
    return status;
  }
  if (auto closed = muxer.value()->close(); closed) {
    return closed;
  }

  stats.packets_read = (video_demuxer ? video_demuxer->packets_read() : 0) +
                       (audio_demuxer ? audio_demuxer->packets_read() : 0);
  // Decoder counters, not the post-filter pull count: `fps=15` halves the frames that reach the
  // encoder, but 30 frames were still decoded and that is what this field means.
  stats.frames_decoded = video.decoder ? video.decoder->frames_decoded() : 0;
  stats.audio_frames_decoded = audio.decoder ? audio.decoder->frames_decoded() : 0;
  stats.frames_converted = video.scaler ? video.scaler->frames_converted() : 0;
  stats.frames_encoded = video.encoder ? video.encoder->frames_in() : 0;
  stats.audio_frames_encoded = audio.encoder ? audio.encoder->frames_in() : 0;
  if (!video.encoder && audio.encoder) {
    // Single audio stream: mirror the counters so callers looking at the generic fields still see
    // progress.
    stats.frames_decoded = stats.audio_frames_decoded;
    stats.frames_encoded = stats.audio_frames_encoded;
  }
  stats.video_streams = video.encoder ? 1 : 0;
  stats.audio_streams = audio.encoder ? 1 : 0;
  stats.wall_ms = elapsed_ms(started);
  return stats;
}

Expected<PipelineStats> transcode_audio(const AudioTranscodeRequest& request) {
  TranscodeRequest audio_request;
  audio_request.input = request.input;
  audio_request.output = request.output;
  audio_request.audio_encode = request.encode;
  audio_request.audio_encode.sample_rate = request.sample_rate;
  audio_request.audio_encode.channels = request.channels;
  audio_request.audio_filter = request.filter;
  audio_request.input_format = request.input_format;
  audio_request.input_options = request.input_options;
  audio_request.include_video = false;
  audio_request.include_audio = true;
  return transcode(audio_request);
}

Expected<PipelineStats> generate(const GenerateRequest& request) {
  PipelineStats stats;
  const auto started = Clock::now();

  auto muxer = Muxer::open(request.output);
  if (!muxer) {
    return muxer.error();
  }

  std::unique_ptr<Encoder> audio_encoder;
  std::unique_ptr<ToneSource> tone;
  int audio_index = -1;
  int audio_rate = 0;
  int audio_frame_size = 0;

  if (request.audio_only || request.with_audio) {
    StreamInfo audio_template;
    audio_template.type = AVMEDIA_TYPE_AUDIO;
    audio_template.time_base = AVRational{1, request.sample_rate};
    audio_template.params.reset(avcodec_parameters_alloc());
    if (!audio_template.params) {
      return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
    }
    audio_template.params->codec_type = AVMEDIA_TYPE_AUDIO;
    audio_template.params->sample_rate = request.sample_rate;
    av_channel_layout_default(&audio_template.params->ch_layout, request.channels);

    EncodeOptions audio_options = request.audio_encode;
    if (audio_options.sample_rate == 0) {
      audio_options.sample_rate = request.sample_rate;
    }
    if (audio_options.channels == 0) {
      audio_options.channels = request.channels;
    }

    auto opened = Encoder::open(audio_template, audio_options);
    if (!opened) {
      return opened.error();
    }
    audio_encoder = opened.take();
    audio_rate = audio_encoder->audio_sample_rate();
    audio_frame_size = audio_encoder->audio_frame_size();
    const int total_frames = static_cast<int>(
        std::ceil(request.seconds * audio_rate / static_cast<double>(audio_frame_size)));
    if (total_frames <= 0) {
      return fail(Code::invalid_argument, "generate: duration must be positive");
    }
    tone = std::make_unique<ToneSource>(audio_rate, audio_encoder->audio_channels(),
                                        audio_encoder->audio_sample_format(), audio_frame_size,
                                        total_frames);
  }

  std::unique_ptr<Encoder> video_encoder;
  std::unique_ptr<SyntheticSource> video_source;
  int video_index = -1;
  if (!request.audio_only) {
    if (request.frames <= 0) {
      return fail(Code::invalid_argument, "generate: frame count must be positive");
    }
    StreamInfo video_template;
    video_template.type = AVMEDIA_TYPE_VIDEO;
    video_template.stream_index = 0;
    video_template.time_base = AVRational{1, request.fps};
    video_template.params.reset(avcodec_parameters_alloc());
    if (!video_template.params) {
      return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
    }
    video_template.params->codec_type = AVMEDIA_TYPE_VIDEO;
    video_template.params->width = request.width;
    video_template.params->height = request.height;
    video_template.params->framerate = AVRational{request.fps, 1};

    auto opened = Encoder::open(video_template, request.encode);
    if (!opened) {
      return opened.error();
    }
    video_encoder = opened.take();
    video_source = std::make_unique<SyntheticSource>(request.width, request.height, request.fps,
                                                     request.frames);
  }

  if (video_encoder) {
    auto index = muxer.value()->add_stream(video_encoder->out_info());
    if (!index) {
      return index.error();
    }
    video_index = index.value();
  }
  if (audio_encoder) {
    auto index = muxer.value()->add_stream(audio_encoder->out_info());
    if (!index) {
      return index.error();
    }
    audio_index = index.value();
  }
  if (auto started_ok = muxer.value()->start(); started_ok) {
    return started_ok;
  }

  bool video_done = video_encoder == nullptr;
  bool audio_done = audio_encoder == nullptr;
  double video_time = 0.0;
  double audio_time = 0.0;

  auto drain = [&](Encoder& encoder, int index, bool audio) -> Status {
    for (;;) {
      auto packet = encoder.pull();
      if (!packet) {
        if (packet.error().code == Code::need_more ||
            packet.error().code == Code::end_of_stream) {
          return ok();
        }
        return packet.error();
      }
      if (auto written = muxer.value()->write(packet.value(), index); written) {
        return written;
      }
      ++stats.packets_written;
      if (audio) {
        ++stats.audio_packets_written;
      }
    }
  };

  while (!video_done || !audio_done) {
    const bool take_video = !video_done && (audio_done || video_time <= audio_time);
    if (take_video) {
      auto frame = video_source->pull();
      if (!frame) {
        if (frame.error().code == Code::end_of_stream) {
          video_done = true;
          continue;
        }
        return frame.error();
      }
      video_time = frame.value()->pts / static_cast<double>(request.fps);
      ++stats.frames_decoded;
      if (auto pushed = video_encoder->push(frame.value()); pushed) {
        return pushed;
      }
      if (auto drained = drain(*video_encoder, video_index, false); drained) {
        return drained;
      }
    } else {
      auto frame = tone->pull();
      if (!frame) {
        if (frame.error().code == Code::end_of_stream) {
          audio_done = true;
          continue;
        }
        return frame.error();
      }
      audio_time = frame.value()->pts / static_cast<double>(audio_rate);
      ++stats.audio_frames_decoded;
      if (auto pushed = audio_encoder->push(frame.value()); pushed) {
        return pushed;
      }
      if (auto drained = drain(*audio_encoder, audio_index, true); drained) {
        return drained;
      }
    }
  }

  if (video_encoder) {
    if (auto flushed = video_encoder->flush(); flushed) {
      return flushed;
    }
    if (auto drained = drain(*video_encoder, video_index, false); drained) {
      return drained;
    }
  }
  if (audio_encoder) {
    if (auto flushed = audio_encoder->flush(); flushed) {
      return flushed;
    }
    if (auto drained = drain(*audio_encoder, audio_index, true); drained) {
      return drained;
    }
  }
  if (auto closed = muxer.value()->close(); closed) {
    return closed;
  }

  stats.frames_encoded = video_encoder ? video_encoder->frames_in() : 0;
  stats.audio_frames_encoded = audio_encoder ? audio_encoder->frames_in() : 0;
  if (!video_encoder && audio_encoder) {
    stats.frames_encoded = stats.audio_frames_encoded;
  }
  stats.video_streams = video_encoder ? 1 : 0;
  stats.audio_streams = audio_encoder ? 1 : 0;
  stats.wall_ms = elapsed_ms(started);
  return stats;
}

Expected<ProbeReport> probe(const std::string& path, const std::string& input_format,
                            const std::string& input_options) {
  auto demuxer = Demuxer::open(path, input_format, input_options);
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

}  // namespace erde
