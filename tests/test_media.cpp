// SPDX-License-Identifier: MIT
#include "harness.hpp"

#include <utility>

#include <erde/error.hpp>
#include <erde/media.hpp>

namespace {

erde::Expected<int> make_value(int value) { return value; }
erde::Expected<int> make_error() {
  return erde::fail(erde::Code::invalid_argument, "nope");
}

}  // namespace

TEST_CASE(expected_holds_value_or_error) {
  auto good = make_value(7);
  CHECK(good.has_value());
  CHECK_EQ(good.value(), 7);

  auto bad = make_error();
  CHECK(!bad.has_value());
  CHECK(bad.error().code == erde::Code::invalid_argument);
  CHECK_EQ(bad.value_or(42), 42);
}

TEST_CASE(frames_are_allocated_and_cloned) {
  auto created = erde::Frame::create();
  CHECK(created.has_value());
  CHECK(!created.value().empty());
  CHECK(created.value().get() != nullptr);

  auto cloned = erde::Frame::clone(created.value());
  CHECK(cloned.has_value());
  CHECK(cloned.value().get() != created.value().get());
}

TEST_CASE(cloning_an_empty_frame_is_rejected) {
  erde::Frame empty;
  CHECK(empty.empty());
  auto cloned = erde::Frame::clone(empty);
  CHECK(!cloned.has_value());
  CHECK(cloned.error().code == erde::Code::invalid_argument);
}

TEST_CASE(pointer_owner_moves_instead_of_copying) {
  erde::PacketPtr a(av_packet_alloc());
  CHECK(static_cast<bool>(a));
  AVFrame* raw = av_frame_alloc();
  erde::FramePtr b(raw);
  erde::FramePtr c(std::move(b));
  CHECK(!b);
  CHECK(static_cast<bool>(c));
  CHECK(c.get() == raw);

  c.reset();
  CHECK(!c);
  CHECK_EQ(c.get(), nullptr);
}

TEST_CASE(error_message_includes_libav_detail) {
  erde::Error error = erde::from_av(AVERROR_INVALIDDATA, "probe failed");
  const std::string text = error.to_string();
  CHECK(text.find("probe failed") != std::string::npos);
  CHECK(text.find("ffmpeg") != std::string::npos);
  CHECK(text.find(erde::av_error_string(AVERROR_INVALIDDATA)) != std::string::npos);
}

TEST_CASE(describes_frames_and_packets) {
  auto frame = erde::Frame::create();
  CHECK(frame.has_value());
  CHECK(frame.value().describe().find("frame[") == 0);

  auto packet = erde::Packet::create();
  CHECK(packet.has_value());
  CHECK(packet.value().describe().find("packet[") == 0);
}

TEST_MAIN()
