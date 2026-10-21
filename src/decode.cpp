// SPDX-License-Identifier: MIT
#include <avpipe/decode.hpp>

namespace avpipe {

Expected<std::unique_ptr<Decoder>> Decoder::open(PacketSource& source, const StreamInfo& info) {
  if (!info.params) {
    return fail(Code::invalid_argument, "Decoder::open: stream has no codec parameters");
  }

  const AVCodec* codec = avcodec_find_decoder(info.params->codec_id);
  if (codec == nullptr) {
    return fail(Code::unsupported,
                "no decoder for codec id " + std::to_string(info.params->codec_id) +
                    " (" + info.codec_name() + ")");
  }

  std::unique_ptr<Decoder> decoder(new Decoder());
  decoder->source_ = &source;
  decoder->info_ = StreamInfo{info.type, info.stream_index, info.time_base, CodecParamsPtr{}};
  decoder->info_.params.reset(avcodec_parameters_alloc());
  if (!decoder->info_.params) {
    return from_av(AVERROR(ENOMEM), "cannot allocate codec parameters");
  }
  if (const int ret = avcodec_parameters_copy(decoder->info_.params.get(), info.params.get());
      ret < 0) {
    return from_av(ret, "cannot copy codec parameters");
  }

  decoder->context_.reset(avcodec_alloc_context3(codec));
  if (!decoder->context_) {
    return from_av(AVERROR(ENOMEM), "cannot allocate decoder context");
  }
  if (const int ret = avcodec_parameters_to_context(decoder->context_.get(), info.params.get());
      ret < 0) {
    return from_av(ret, "cannot apply codec parameters to decoder");
  }
  decoder->context_->pkt_timebase = info.time_base;
  if (const int ret = avcodec_open2(decoder->context_.get(), codec, nullptr); ret < 0) {
    return from_av(ret, std::string("cannot open decoder for ") + codec->name);
  }

  return decoder;
}

Decoder::~Decoder() = default;

Expected<Frame> Decoder::pull() {
  if (finished_) {
    return fail(Code::end_of_stream, "decoder finished");
  }

  for (;;) {
    Frame frame;
    if (auto created = Frame::create(); created) {
      frame = created.take();
    } else {
      return created.error();
    }

    const int ret = avcodec_receive_frame(context_.get(), frame.get());
    if (ret == 0) {
      ++frames_;
      return frame;
    }
    if (ret == AVERROR_EOF) {
      finished_ = true;
      return fail(Code::end_of_stream, "decoder reached EOF");
    }
    if (ret != AVERROR(EAGAIN)) {
      return from_av(ret, "avcodec_receive_frame");
    }

    // Nothing to receive: feed one more packet, or start draining at end of input.
    if (pending_) {
      const int sent = avcodec_send_packet(context_.get(), pending_.get());
      pending_.reset();
      if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF) {
        return from_av(sent, "avcodec_send_packet");
      }
      continue;
    }

    if (draining_) {
      // Drain mode entered and nothing came out: the decoder is done.
      finished_ = true;
      return fail(Code::end_of_stream, "decoder drained");
    }

    auto packet = source_->pull();
    if (!packet) {
      if (packet.error().code == Code::end_of_stream) {
        draining_ = true;
        const int sent = avcodec_send_packet(context_.get(), nullptr);
        if (sent < 0 && sent != AVERROR_EOF) {
          return from_av(sent, "cannot start decoder drain");
        }
        continue;
      }
      if (packet.error().code == Code::need_more) {
        return packet.error();
      }
      return packet.error();
    }

    const int sent = avcodec_send_packet(context_.get(), packet.value().get());
    if (sent == AVERROR(EAGAIN)) {
      // The decoder wants its pending output taken first; keep the packet for the next round.
      pending_ = packet.take();
      continue;
    }
    if (sent == AVERROR_EOF) {
      draining_ = true;
      continue;
    }
    if (sent < 0) {
      return from_av(sent, "avcodec_send_packet");
    }
  }
}

}  // namespace avpipe
