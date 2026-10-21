// SPDX-License-Identifier: MIT
#include <avpipe/mux.hpp>

namespace avpipe {

Expected<std::unique_ptr<Muxer>> Muxer::open(const std::string& path) {
  std::unique_ptr<Muxer> muxer(new Muxer());
  muxer->path_ = path;

  AVFormatContext* raw = nullptr;
  if (const int ret = avformat_alloc_output_context2(&raw, nullptr, nullptr, path.c_str());
      ret < 0 || raw == nullptr) {
    return from_av(ret < 0 ? ret : AVERROR(EINVAL),
                   "cannot create output context for '" + path + "'");
  }
  muxer->format_.reset(raw);
  return muxer;
}

Expected<int> Muxer::add_stream(const StreamInfo& encoded) {
  if (started_) {
    return fail(Code::invalid_argument, "Muxer::add_stream: header already written");
  }
  if (!encoded.params) {
    return fail(Code::invalid_argument, "Muxer::add_stream: no encoder parameters");
  }

  AVStream* stream = avformat_new_stream(format_.get(), nullptr);
  if (stream == nullptr) {
    return from_av(AVERROR(ENOMEM), "cannot add output stream");
  }
  if (const int ret = avcodec_parameters_copy(stream->codecpar, encoded.params.get()); ret < 0) {
    return from_av(ret, "cannot copy encoder parameters into the muxer");
  }
  stream->codecpar->codec_tag = 0;
  stream->time_base = encoded.time_base;
  // avg_frame_rate only makes sense for video; setting it on audio makes some muxers complain.
  if (encoded.type == AVMEDIA_TYPE_VIDEO && encoded.params->framerate.num > 0 &&
      encoded.params->framerate.den > 0) {
    stream->avg_frame_rate = encoded.params->framerate;
  }

  const int index = static_cast<int>(streams_.size());
  streams_.push_back(OutputStream{stream, encoded.time_base});
  return index;
}

Status Muxer::start() {
  if (started_) {
    return ok();
  }
  if (streams_.empty()) {
    return fail(Code::invalid_argument, "Muxer::start: no streams were added");
  }

  if ((format_->oformat->flags & AVFMT_NOFILE) == 0) {
    if (const int ret = avio_open(&format_->pb, path_.c_str(), AVIO_FLAG_WRITE); ret < 0) {
      return from_av(ret, "cannot open '" + path_ + "' for writing");
    }
  }
  if (const int ret = avformat_write_header(format_.get(), nullptr); ret < 0) {
    return from_av(ret, "avformat_write_header");
  }
  started_ = true;
  return ok();
}

Muxer::~Muxer() {
  if (!closed_) {
    // Best effort: never throw from a destructor, but still try to leave a playable file.
    close();
  }
}

Status Muxer::write(const Packet& packet) { return write(packet, 0); }

Status Muxer::write(const Packet& packet, int stream_index) {
  if (closed_) {
    return fail(Code::invalid_argument, "Muxer::write: muxer already closed");
  }
  if (!started_) {
    return fail(Code::invalid_argument, "Muxer::write: start() was not called");
  }
  if (packet.empty()) {
    return fail(Code::invalid_argument, "Muxer::write: empty packet");
  }
  if (stream_index < 0 || stream_index >= stream_count()) {
    return fail(Code::invalid_argument,
                "Muxer::write: stream " + std::to_string(stream_index) + " was not registered");
  }

  PacketPtr copy(av_packet_clone(packet.get()));
  if (!copy) {
    return from_av(AVERROR(ENOMEM), "av_packet_clone");
  }

  OutputStream& out = streams_[static_cast<std::size_t>(stream_index)];
  // The encoder counts in its own time base; the container decides its own. Rescale rather than
  // letting timestamps drift by the ratio of the two.
  av_packet_rescale_ts(copy.get(), out.encoder_time_base, out.stream->time_base);
  copy->stream_index = out.stream->index;

  if (const int ret = av_interleaved_write_frame(format_.get(), copy.get()); ret < 0) {
    return from_av(ret, "av_interleaved_write_frame");
  }
  ++packets_written_;
  return ok();
}

Status Muxer::close() {
  if (closed_) {
    return ok();
  }
  closed_ = true;
  if (!format_) {
    return ok();
  }
  if (started_) {
    if (const int ret = av_write_trailer(format_.get()); ret < 0) {
      return from_av(ret, "av_write_trailer");
    }
  }
  if (format_->pb != nullptr && (format_->oformat->flags & AVFMT_NOFILE) == 0) {
    avio_closep(&format_->pb);
  }
  return ok();
}

}  // namespace avpipe
