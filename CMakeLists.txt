cmake_minimum_required(VERSION 3.16)
project(erde VERSION 0.3.0 LANGUAGES CXX)

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
target_include_directories(erde_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(erde_core PUBLIC PkgConfig::LIBAV)
target_compile_options(erde_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(erde src/cli/main.cpp)
target_link_libraries(erde PRIVATE erde_core)
target_compile_options(erde PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(tests)

install(TARGETS erde RUNTIME DESTINATION bin)
