// SPDX-License-Identifier: MIT
#ifndef AVPIPE_FFMPEG_HPP
#define AVPIPE_FFMPEG_HPP

// Single place that pulls in the C headers of libav* inside `extern "C"`.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#endif  // AVPIPE_FFMPEG_HPP
