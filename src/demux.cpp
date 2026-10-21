// SPDX-License-Identifier: MIT
#include <avpipe/demux.hpp>

#include <utility>

namespace avpipe {

Expected<std::unique_ptr<Demuxer>> Demuxer::open(const std::string& path) {
  std::unique_ptr<Demuxer> demux(new Demuxer());

  AVFormatContext* raw = nullptr;
  if (const int ret = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); ret < 0) {
    return from_av(ret, "cannot open '" + path + "'");
  }
  demux->format_.reset(raw);

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

}  // namespace avpipe
