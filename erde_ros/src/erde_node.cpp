// SPDX-License-Identifier: MIT
#include <erde_ros/erde_node.hpp>

#include <lifecycle_msgs/msg/state.hpp>

#include <utility>

namespace erde_ros {

namespace {

/// Copies one erde::Error into a lifecycle-node log message and returns FAILURE.
CallbackReturn fail(rclcpp_lifecycle::LifecycleNode& node, const erde::Error& error) {
  RCLCPP_ERROR(node.get_logger(), "erde: %s", error.to_string().c_str());
  return CallbackReturn::FAILURE;
}

}  // namespace

ErdeNode::ErdeNode()
    : rclcpp_lifecycle::LifecycleNode(
          "erde",
          rclcpp::NodeOptions().allow_undeclared_parameters(false).automatically_declare_parameters_from_overrides(
              false)),
      diagnostics_(this) {
  // Declared here so `ros2 param list` works before configure as well.
  declare_parameter<std::string>("input", "");
  declare_parameter<std::string>("input_format", "");
  declare_parameter<std::string>("input_options", "");
  declare_parameter<std::string>("filter", "");
  declare_parameter<int>("width", 0);
  declare_parameter<int>("height", 0);
  declare_parameter<std::string>("codec", "libx264");
  declare_parameter<int>("bitrate", 2'000'000);
  declare_parameter<int>("gop", 12);
  declare_parameter<int>("fps", 30);
  declare_parameter<std::string>("frame_id", "camera");
  declare_parameter<std::string>("topic", "~/image/compressed");
  declare_parameter<bool>("autostart", true);

  diagnostics_.setHardwareID("erde");
  diagnostics_.add("pipeline", this, &ErdeNode::produce_diagnostics);

  stats_service_ = create_service<std_srvs::srv::Trigger>(
      "~/stats", [this](const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
                        std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        handle_stats(std::move(request), std::move(response));
      });
}

CallbackReturn ErdeNode::on_configure(const rclcpp_lifecycle::State& /*state*/) {
  const auto input = get_parameter("input").as_string();
  if (input.empty()) {
    RCLCPP_ERROR(get_logger(), "'input' must be set to a file or a device path");
    return CallbackReturn::FAILURE;
  }
  const auto input_format = get_parameter("input_format").as_string();
  const auto input_options = get_parameter("input_options").as_string();
  const auto filter_description = get_parameter("filter").as_string();
  const int width = get_parameter("width").as_int();
  const int height = get_parameter("height").as_int();

  auto demuxer = erde::Demuxer::open(input, input_format, input_options);
  if (!demuxer) {
    return fail(*this, demuxer.error());
  }
  demuxer_ = demuxer.take();
  if (auto selected = demuxer_->select_stream(AVMEDIA_TYPE_VIDEO); selected) {
    return fail(*this, selected);
  }
  const erde::StreamInfo* info = demuxer_->find(AVMEDIA_TYPE_VIDEO);
  if (info == nullptr) {
    RCLCPP_ERROR(get_logger(), "'%s' has no video stream", input.c_str());
    return CallbackReturn::FAILURE;
  }

  auto decoder = erde::Decoder::open(*demuxer_, *info);
  if (!decoder) {
    return fail(*this, decoder.error());
  }
  decoder_ = decoder.take();
  frames_ = decoder_.get();

  if (!filter_description.empty()) {
    auto opened = erde::FilterSource::open(*frames_, filter_description, info->time_base);
    if (!opened) {
      return fail(*this, opened.error());
    }
    filter_ = opened.take();
    frames_ = filter_.get();
  }
  if (width > 0 && height > 0) {
    auto opened = erde::ScalingSource::open(*frames_, width, height);
    if (!opened) {
      return fail(*this, opened.error());
    }
    scaler_ = opened.take();
    frames_ = scaler_.get();
  }

  erde::EncodeOptions options;
  options.codec = get_parameter("codec").as_string();
  options.bit_rate = get_parameter("bitrate").as_int();
  options.gop = get_parameter("gop").as_int();
  options.fps = get_parameter("fps").as_int();

  // The encoder is sized from the *stream*; when a scaler is in front, its geometry wins.
  // StreamInfo owns its AVCodecParameters through a move-only pointer, so it is built field by
  // field rather than copied.
  erde::StreamInfo encoder_input;
  encoder_input.type = info->type;
  encoder_input.stream_index = info->stream_index;
  encoder_input.time_base = info->time_base;
  encoder_input.params.reset(avcodec_parameters_alloc());
  if (!encoder_input.params) {
    RCLCPP_ERROR(get_logger(), "cannot allocate codec parameters");
    return CallbackReturn::FAILURE;
  }
  if (avcodec_parameters_copy(encoder_input.params.get(), info->params.get()) < 0) {
    RCLCPP_ERROR(get_logger(), "cannot copy codec parameters");
    return CallbackReturn::FAILURE;
  }
  if (width > 0 && height > 0) {
    encoder_input.params->width = width;
    encoder_input.params->height = height;
  }

  auto encoder = erde::Encoder::open(encoder_input, options);
  if (!encoder) {
    return fail(*this, encoder.error());
  }
  encoder_ = encoder.take();
  codec_name_ = encoder_->codec_name();

  publisher_ = create_publisher<sensor_msgs::msg::CompressedImage>(
      get_parameter("topic").as_string(), rclcpp::QoS(10).reliable());

  RCLCPP_INFO(get_logger(), "configured: %s -> %s (%s)", input.c_str(), codec_name_.c_str(),
              info->describe().c_str());
  return CallbackReturn::SUCCESS;
}

CallbackReturn ErdeNode::on_activate(const rclcpp_lifecycle::State& /*state*/) {
  if (!encoder_ || !publisher_) {
    RCLCPP_ERROR(get_logger(), "activate before configure");
    return CallbackReturn::FAILURE;
  }
  publisher_->on_activate();
  const auto period = std::chrono::duration<double>(1.0 / get_parameter("fps").as_int());
  timer_ = create_wall_timer(std::chrono::duration_cast<std::chrono::nanoseconds>(period),
                             [this]() { pull_and_publish(); });
  diagnostics_.setPeriod(1.0);
  diagnostics_.force_update();
  RCLCPP_INFO(get_logger(), "activated at %.2f Hz", 1.0 / period.count());
  return CallbackReturn::SUCCESS;
}

CallbackReturn ErdeNode::on_deactivate(const rclcpp_lifecycle::State& /*state*/) {
  if (timer_) {
    timer_.reset();
  }
  if (encoder_) {
    if (auto flushed = encoder_->flush(); flushed) {
      RCLCPP_WARN(get_logger(), "flush failed: %s", flushed.to_string().c_str());
    }
    drain_encoder();
  }
  if (publisher_) {
    publisher_->on_deactivate();
  }
  RCLCPP_INFO(get_logger(), "deactivated (%ld frames published)", frames_published_);
  return CallbackReturn::SUCCESS;
}

CallbackReturn ErdeNode::on_cleanup(const rclcpp_lifecycle::State& /*state*/) {
  timer_.reset();
  publisher_.reset();
  encoder_.reset();
  scaler_.reset();
  filter_.reset();
  decoder_.reset();
  demuxer_.reset();
  frames_ = nullptr;
  source_finished_ = false;
  return CallbackReturn::SUCCESS;
}

CallbackReturn ErdeNode::on_shutdown(const rclcpp_lifecycle::State& state) {
  if (state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE) {
    on_deactivate(state);
  }
  on_cleanup(state);
  return CallbackReturn::SUCCESS;
}

void ErdeNode::drain_encoder() {
  for (;;) {
    auto packet = encoder_->pull();
    if (!packet) {
      return;
    }
    if (!publisher_ || !publisher_->is_activated()) {
      return;
    }
    sensor_msgs::msg::CompressedImage message;
    message.header.stamp = now();
    message.header.frame_id = get_parameter("frame_id").as_string();
    message.format = codec_name_;
    message.data.assign(packet.value()->data, packet.value()->data + packet.value()->size);
    publisher_->publish(message);
    ++packets_published_;
  }
}

void ErdeNode::pull_and_publish() {
  if (source_finished_) {
    return;
  }

  auto frame = frames_->pull();
  if (!frame) {
    if (frame.error().code == erde::Code::end_of_stream) {
      source_finished_ = true;
      RCLCPP_INFO(get_logger(), "input finished");
      return;
    }
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000, "pull failed: %s",
                         frame.error().to_string().c_str());
    ++pull_errors_;
    return;
  }

  if (auto pushed = encoder_->push(frame.value()); pushed) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000, "encode failed: %s",
                         pushed.to_string().c_str());
    return;
  }
  const auto before = packets_published_;
  drain_encoder();
  ++frames_pulled_;
  if (packets_published_ > before) {
    ++frames_published_;
  }
  // No packet this tick is normal: x264 keeps frames for lookahead/B-frames and emits them on a
  // later push. Only a failed pull counts as a problem.
}

void ErdeNode::produce_diagnostics(diagnostic_updater::DiagnosticStatusWrapper& status) {
  const bool healthy = encoder_ != nullptr && !source_finished_;
  status.summary(healthy ? diagnostic_msgs::msg::DiagnosticStatus::OK
                         : diagnostic_msgs::msg::DiagnosticStatus::WARN,
                 healthy ? "pipeline running" : "pipeline idle or finished");
  status.add("codec", codec_name_.empty() ? "none" : codec_name_);
  status.add("frames_pulled", frames_pulled_);
  status.add("frames_published", frames_published_);
  status.add("pull_errors", pull_errors_);
  status.add("packets_published", packets_published_);
  status.add("source_finished", source_finished_);
  status.add("demuxer_packets_read", demuxer_ ? demuxer_->packets_read() : 0);
  status.add("decoder_frames", decoder_ ? decoder_->frames_decoded() : 0);
}

void ErdeNode::handle_stats(const std::shared_ptr<std_srvs::srv::Trigger::Request> /*request*/,
                            std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
  response->success = encoder_ != nullptr;
  response->message = "pulled=" + std::to_string(frames_pulled_) +
                      " published=" + std::to_string(frames_published_) +
                      " errors=" + std::to_string(pull_errors_) +
                      " packets=" + std::to_string(packets_published_) +
                      " codec=" + (codec_name_.empty() ? "none" : codec_name_);
}

}  // namespace erde_ros
