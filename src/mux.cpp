// SPDX-License-Identifier: MIT
#include <avpipe/mux.hpp>

namespace avpipe {

Expected<std::unique_ptr<Muxer>> Muxer::open(const std::string& path, const StreamInfo& encoded) {
  if (!encoded.params) {
    return fail(Code::invalid_argument, "Muxer::open: no encoder parameters");
  }

  std::unique_ptr<Muxer> muxer(new Muxer());
  muxer->path_ = path;
  muxer->encoder_time_base_ = encoded.time_base;

  AVFormatContext* raw = nullptr;
  if (const int ret = avformat_alloc_output_context2(&raw, nullptr, nullptr, path.c_str());
      ret < 0 || raw == nullptr) {
    return from_av(ret < 0 ? ret : AVERROR(EINVAL),
                   "cannot create output context for '" + path + "'");
  }
  muxer->format_.reset(raw);

  AVStream* stream = avformat_new_stream(muxer->format_.get(), nullptr);
  if (stream == nullptr) {
    return from_av(AVERROR(ENOMEM), "cannot add output stream");
  }
  if (const int ret = avcodec_parameters_copy(stream->codecpar, encoded.params.get()); ret < 0) {
    return from_av(ret, "cannot copy encoder parameters into the muxer");
  }
  stream->codecpar->codec_tag = 0;
  stream->time_base = encoded.time_base;
  if (encoded.params->framerate.num > 0 && encoded.params->framerate.den > 0) {
    stream->avg_frame_rate = encoded.params->framerate;
  }
  muxer->stream_ = stream;

  if ((muxer->format_->oformat->flags & AVFMT_NOFILE) == 0) {
    if (const int ret = avio_open(&muxer->format_->pb, path.c_str(), AVIO_FLAG_WRITE); ret < 0) {
      return from_av(ret, "cannot open '" + path + "' for writing");
    }
  }

  if (const int ret = avformat_write_header(muxer->format_.get(), nullptr); ret < 0) {
    return from_av(ret, "avformat_write_header");
  }
  muxer->header_written_ = true;
  return muxer;
}

Muxer::~Muxer() {
  if (!closed_) {
    // Best effort: never throw from a destructor, but still try to leave a playable file.
    close();
  }
}

Status Muxer::write(const Packet& packet) {
  if (closed_) {
    return fail(Code::invalid_argument, "Muxer::write: muxer already closed");
  }
  if (packet.empty()) {
    return fail(Code::invalid_argument, "Muxer::write: empty packet");
  }

  PacketPtr copy(av_packet_clone(packet.get()));
  if (!copy) {
    return from_av(AVERROR(ENOMEM), "av_packet_clone");
  }

  // The encoder counts in its own time base; the container decides its own. Rescale rather than
  // letting timestamps drift by the ratio of the two.
  av_packet_rescale_ts(copy.get(), encoder_time_base_, stream_->time_base);
  copy->stream_index = stream_->index;

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
  if (header_written_) {
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
