cmake_minimum_required(VERSION 3.16)
project(erde VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# libav* is found through pkg-config: the six libraries always ship .pc files on Linux, and it
# keeps the build file short compared to hand-written Find modules.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libswscale
  libswresample
  libavfilter
)

add_library(erde_core STATIC
  src/media.cpp
  src/config.cpp
  src/demux.cpp
  src/decode.cpp
  src/encode.cpp
  src/scale.cpp
  src/filter.cpp
  src/audio.cpp
  src/mux.cpp
  src/pipeline.cpp
)
target_include_directories(erde_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(erde_core PUBLIC PkgConfig::LIBAV)
target_compile_options(erde_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(erde src/cli/main.cpp)
target_link_libraries(erde PRIVATE erde_core)
target_compile_options(erde PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(tests)

# Install + export, so an ament_cmake package (erde_ros) can `find_package(erde)`.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS erde_core EXPORT erdeTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS erde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(EXPORT erdeTargets
  FILE erdeTargets.cmake
  NAMESPACE erde::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/erde
)

configure_package_config_file(cmake/erde-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/erde-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/erde
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/erde-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/erde-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/erde-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/erde
)
