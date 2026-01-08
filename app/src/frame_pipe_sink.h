#ifndef SC_FRAME_PIPE_SINK_H
#define SC_FRAME_PIPE_SINK_H

#include "common.h"

#include <stdbool.h>
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>

#include "trait/frame_sink.h"

/**
 * Frame pipe sink - outputs decoded YUV420P frames to a named pipe.
 *
 * Protocol (all integers little-endian):
 *
 *   Dimension packet (12 bytes) - sent on start and dimension change:
 *     "YUV4" (4B) | width (4B) | height (4B)
 *
 *   Frame packet - sent for every frame:
 *     PTS in microseconds (8B) | YUV420P data (width * height * 1.5 bytes)
 *
 * Reader logic: read 4 bytes, if == "YUV4" it's a dimension packet (read 8
 * more bytes for w/h), otherwise it's the start of PTS (read 4 more bytes,
 * then frame data). "YUV4" can never appear as valid PTS (would be ~47000 years).
 */
struct sc_frame_pipe_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    char *pipe_path;
    int fd; // File descriptor for the named pipe
    bool stopped;

    // Frame format info
    int width;
    int height;
    AVRational time_base; // For PTS conversion to microseconds

    // Reusable buffer for frames with padding (avoids per-frame allocation)
    uint8_t *frame_buf;
    size_t frame_buf_size;
};

bool
sc_frame_pipe_sink_init(struct sc_frame_pipe_sink *fps, const char *pipe_path);

void
sc_frame_pipe_sink_destroy(struct sc_frame_pipe_sink *fps);

#endif
