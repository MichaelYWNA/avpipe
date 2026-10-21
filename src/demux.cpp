// SPDX-License-Identifier: MIT
#include <erde/demux.hpp>

#include <utility>

namespace erde {

Expected<std::unique_ptr<Demuxer>> Demuxer::open(const std::string& path,
                                                 const std::string& format_name,
                                                 const std::string& options) {
  std::unique_ptr<Demuxer> demux(new Demuxer());

  const AVInputFormat* input_format = nullptr;
  if (!format_name.empty()) {
    input_format = av_find_input_format(format_name.c_str());
    if (input_format == nullptr) {
      return fail(Code::unsupported, "unknown input format '" + format_name + "'");
    }
  }

  AVDictionary* raw_options = nullptr;
  if (!options.empty()) {
    if (const int ret = av_dict_parse_string(&raw_options, options.c_str(), "=", ",", 0);
        ret < 0) {
      return from_av(ret, "cannot parse input options '" + options + "'");
    }
  }

  AVFormatContext* raw = nullptr;
  const int opened = avformat_open_input(&raw, path.c_str(), input_format, &raw_options);
  if (opened < 0) {
    av_dict_free(&raw_options);
    return from_av(opened, "cannot open '" + path + "'");
  }
  demux->format_.reset(raw);

  // libav returns the options it did *not* consume; a silently ignored option turns into a
  // silently wrong pipeline (30 fps requested, 15 fps delivered), so treat it as an error.
  if (raw_options != nullptr) {
    int leftovers = av_dict_count(raw_options);
    av_dict_free(&raw_options);
    if (leftovers > 0) {
      return fail(Code::invalid_argument, "unused input option(s) in '" + options +
                                              "' - check the names for this input format");
    }
  }
  av_dict_free(&raw_options);

  if (const int ret = avformat_find_stream_info(demux->format_.get(), nullptr); ret < 0) {
    return from_av(ret, "cannot read stream info of '" + path + "'");
  }

  if (auto p = Packet::create(); p) {
    demux->scratch_ = std::move(p.value());
  } else {
    return p.error();
  }

  demux->streams_.reserve(demux->format_->nb_streams);
  for (unsigned i = 0; i < demux->format_->nb_streams; ++i) {
    AVStream* stream = demux->format_->streams[i];
    StreamInfo info;
    info.type = stream->codecpar->codec_type;
    info.stream_index = static_cast<int>(i);
    info.time_base = stream->time_base;
    info.params.reset(avcodec_parameters_alloc());
    if (!info.params) {
      return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
    }
    if (const int ret = avcodec_parameters_copy(info.params.get(), stream->codecpar); ret < 0) {
      return from_av(ret, "cannot copy codec parameters");
    }
    demux->streams_.push_back(std::move(info));
  }

  return demux;
}

Demuxer::~Demuxer() = default;

Status Demuxer::select_stream(AVMediaType type) {
  for (const StreamInfo& info : streams_) {
    if (info.type == type) {
      selected_ = info.stream_index;
      return ok();
    }
  }
  return fail(Code::unsupported, std::string("no ") + av_get_media_type_string(type) + " stream");
}

const StreamInfo* Demuxer::find(AVMediaType type) const {
  for (const StreamInfo& info : streams_) {
    if (info.type == type) {
      return &info;
    }
  }
  return nullptr;
}

Expected<Packet> Demuxer::pull() {
  if (eof_) {
    return fail(Code::end_of_stream, "demuxer at EOF");
  }

  for (;;) {
    const int ret = av_read_frame(format_.get(), scratch_.get());
    if (ret == AVERROR_EOF) {
      eof_ = true;
      return fail(Code::end_of_stream, "demuxer at EOF");
    }
    if (ret == AVERROR(EAGAIN)) {
      return fail(Code::need_more, "demuxer needs more data");
    }
    if (ret < 0) {
      return from_av(ret, "av_read_frame");
    }

    ++packets_read_;
    // Hand the scratch packet to the caller. A new AVPacket must be allocated for the next
    // read, otherwise the caller and av_read_frame() would share one buffer.
    if (selected_ >= 0 && scratch_->stream_index != selected_) {
      av_packet_unref(scratch_.get());
      continue;
    }

    Packet out;
    if (auto created = Packet::create(); created) {
      out = created.take();
    } else {
      return created.error();
    }
    av_packet_move_ref(out.get(), scratch_.get());

    if (auto next = Packet::create(); next) {
      scratch_ = next.take();
    } else {
      return next.error();
    }
    return out;
  }
}

double Demuxer::duration_seconds() const {
  if (!format_ || format_->duration == AV_NOPTS_VALUE) {
    return 0.0;
  }
  return static_cast<double>(format_->duration) / AV_TIME_BASE;
}

std::string Demuxer::format_name() const {
  if (!format_ || format_->iformat == nullptr) {
    return "unknown";
  }
  return format_->iformat->name;
}

bool Demuxer::is_live() const {
  if (!format_) {
    return false;
  }
  const bool infinite = format_->duration == AV_NOPTS_VALUE;
  const bool realtime = (format_->iformat->flags & AVFMT_NOFILE) != 0 ||
                        (format_->flags & AVFMT_FLAG_NONBLOCK) != 0;
  return infinite || realtime;
}

}  // namespace erde
