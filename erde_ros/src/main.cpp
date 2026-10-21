// SPDX-License-Identifier: MIT
#include <memory>

#include <lifecycle_msgs/msg/transition.hpp>
#include <rclcpp/rclcpp.hpp>

#include <erde_ros/erde_node.hpp>

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<erde_ros::ErdeNode>();

  // A lifecycle node normally waits for `ros2 lifecycle set`. For a demo and for CI it is more
  // useful to come up configured and active unless the caller opts out.
  if (node->get_parameter("autostart").as_bool()) {
    node->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_CONFIGURE);
    node->trigger_transition(lifecycle_msgs::msg::Transition::TRANSITION_ACTIVATE);
  }

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node->get_node_base_interface());
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
