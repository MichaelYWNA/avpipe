# SPDX-License-Identifier: MIT
#
# Launch the erde lifecycle node on a file or a device.
#
#   ros2 launch erde_ros erde.launch.py input:=/dev/video0 input_format:=v4l2 \
#       input_options:='video_size=1280x720,framerate=30'
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    arguments = [
        DeclareLaunchArgument('input', description='file path or device, e.g. /dev/video0'),
        DeclareLaunchArgument('input_format', default_value='',
                              description='demuxer for devices: v4l2, pulse, alsa (empty = probe)'),
        DeclareLaunchArgument('input_options', default_value='',
                              description="device options, e.g. 'video_size=1280x720,framerate=30'"),
        DeclareLaunchArgument('filter', default_value='',
                              description="libavfilter chain, e.g. 'fps=15,scale=640:360'"),
        DeclareLaunchArgument('codec', default_value='libx264'),
        DeclareLaunchArgument('fps', default_value='30',
                              description='pull rate; keep it at or above the source frame rate'),
        DeclareLaunchArgument('topic', default_value='~/image/compressed'),
    ]

    node = Node(
        package='erde_ros',
        executable='erde_node',
        name='erde',
        output='screen',
        parameters=[{
            'input': LaunchConfiguration('input'),
            'input_format': LaunchConfiguration('input_format'),
            'input_options': LaunchConfiguration('input_options'),
            'filter': LaunchConfiguration('filter'),
            'codec': LaunchConfiguration('codec'),
            'fps': LaunchConfiguration('fps'),
            'topic': LaunchConfiguration('topic'),
        }],
    )

    return LaunchDescription(arguments + [node])
