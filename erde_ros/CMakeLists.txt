cmake_minimum_required(VERSION 3.16)
project(erde_ros)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(diagnostic_updater REQUIRED)

# erde is not a rosdep key: install it first (cmake --install ... --prefix PREFIX) and make sure
# PREFIX is on CMAKE_PREFIX_PATH / AMENT_PREFIX_PATH.
find_package(erde REQUIRED)

add_executable(erde_node src/erde_node.cpp src/main.cpp)
target_include_directories(erde_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(erde_node erde::erde_core)
ament_target_dependencies(erde_node rclcpp rclcpp_lifecycle sensor_msgs std_srvs
                          diagnostic_updater)

install(TARGETS erde_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
