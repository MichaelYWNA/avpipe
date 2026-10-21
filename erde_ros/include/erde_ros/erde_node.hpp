// SPDX-License-Identifier: MIT
//
// erde_node: a lifecycle node that owns an erde pipeline and publishes encoded frames.
//
// The architecture is deliberately *not* a ros2_control controller chain: the node owns one
// pipeline object graph and drives it from a wall timer, which keeps the media stages ordinary
// C++ classes that can be unit-tested without a ROS graph. Configuration happens in
// on_configure, the timer only exists between on_activate and on_deactivate.
#include <chrono>
#include <memory>
#include <string>

#include <diagnostic_updater/diagnostic_updater.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rclcpp_lifecycle/lifecycle_publisher.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <erde/decode.hpp>
#include <erde/demux.hpp>
#include <erde/encode.hpp>
#include <erde/error.hpp>
#include <erde/filter.hpp>
#include <erde/scale.hpp>
#include <erde/stage.hpp>

namespace erde_ros {

using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

class ErdeNode : public rclcpp_lifecycle::LifecycleNode {
 public:
  ErdeNode();

  CallbackReturn on_configure(const rclcpp_lifecycle::State& state) override;
  CallbackReturn on_activate(const rclcpp_lifecycle::State& state) override;
  CallbackReturn on_deactivate(const rclcpp_lifecycle::State& state) override;
  CallbackReturn on_cleanup(const rclcpp_lifecycle::State& state) override;
  CallbackReturn on_shutdown(const rclcpp_lifecycle::State& state) override;

 private:
  void pull_and_publish();
  void drain_encoder();
  void produce_diagnostics(diagnostic_updater::DiagnosticStatusWrapper& status);
  void handle_stats(const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
                    std::shared_ptr<std_srvs::srv::Trigger::Response> response);

  // pipeline (only alive between configure and cleanup)
  std::unique_ptr<erde::Demuxer> demuxer_;
  std::unique_ptr<erde::Decoder> decoder_;
  std::unique_ptr<erde::FilterSource> filter_;
  std::unique_ptr<erde::ScalingSource> scaler_;
  std::unique_ptr<erde::Encoder> encoder_;
  erde::FrameSource* frames_ = nullptr;

  rclcpp_lifecycle::LifecyclePublisher<sensor_msgs::msg::CompressedImage>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stats_service_;
  diagnostic_updater::Updater diagnostics_;

  std::string codec_name_;
  int64_t frames_pulled_ = 0;
  int64_t frames_published_ = 0;
  int64_t pull_errors_ = 0;
  int64_t packets_published_ = 0;
  bool source_finished_ = false;
};

}  // namespace erde_ros
