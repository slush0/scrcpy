#include "frame_pipe_sink.h"

#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/log.h"

/** Downcast frame_sink to sc_frame_pipe_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_frame_pipe_sink, frame_sink)

// See frame_pipe_sink.h for protocol documentation

static bool
write_header(struct sc_frame_pipe_sink *fps, int width, int height) {
    uint8_t header[12];
    header[0] = 'Y';
    header[1] = 'U';
    header[2] = 'V';
    header[3] = '4';
    // Width (little-endian)
    header[4] = width & 0xFF;
    header[5] = (width >> 8) & 0xFF;
    header[6] = (width >> 16) & 0xFF;
    header[7] = (width >> 24) & 0xFF;
    // Height (little-endian)
    header[8] = height & 0xFF;
    header[9] = (height >> 8) & 0xFF;
    header[10] = (height >> 16) & 0xFF;
    header[11] = (height >> 24) & 0xFF;

    ssize_t w = write(fps->fd, header, sizeof(header));
    if (w != sizeof(header)) {
        LOGE("Frame pipe sink: failed to write header: %s", strerror(errno));
        return false;
    }

    fps->width = width;
    fps->height = height;
    LOGI("Frame pipe sink: header written %dx%d", width, height);
    return true;
}

// Write PTS in microseconds (8 bytes, little-endian)
static bool
write_pts(struct sc_frame_pipe_sink *fps, int64_t pts_us) {
    uint8_t buf[8];
    buf[0] = pts_us & 0xFF;
    buf[1] = (pts_us >> 8) & 0xFF;
    buf[2] = (pts_us >> 16) & 0xFF;
    buf[3] = (pts_us >> 24) & 0xFF;
    buf[4] = (pts_us >> 32) & 0xFF;
    buf[5] = (pts_us >> 40) & 0xFF;
    buf[6] = (pts_us >> 48) & 0xFF;
    buf[7] = (pts_us >> 56) & 0xFF;

    ssize_t w = write(fps->fd, buf, sizeof(buf));
    if (w != sizeof(buf)) {
        if (errno == EPIPE) {
            LOGD("Frame pipe sink: reader disconnected");
        } else {
            LOGW("Frame pipe sink: write error PTS: %s", strerror(errno));
        }
        return false;
    }
    return true;
}

static bool
write_frame_data(struct sc_frame_pipe_sink *fps, const AVFrame *frame) {
    int width = frame->width;
    int height = frame->height;
    int uv_width = width / 2;
    int uv_height = height / 2;

    // Total frame size for YUV420P
    size_t y_size = (size_t)width * height;
    size_t uv_size = (size_t)uv_width * uv_height;
    size_t frame_size = y_size + uv_size * 2;

    // Check if we can write directly (no padding in frame)
    bool can_write_direct =
        frame->linesize[0] == width &&
        frame->linesize[1] == uv_width &&
        frame->linesize[2] == uv_width;

    if (can_write_direct) {
        // Fast path: write planes directly with writev (3 syscalls -> 1)
        struct iovec iov[3];
        iov[0].iov_base = frame->data[0];
        iov[0].iov_len = y_size;
        iov[1].iov_base = frame->data[1];
        iov[1].iov_len = uv_size;
        iov[2].iov_base = frame->data[2];
        iov[2].iov_len = uv_size;

        ssize_t written = writev(fps->fd, iov, 3);
        if (written != (ssize_t)frame_size) {
            if (errno == EPIPE) {
                LOGD("Frame pipe sink: reader disconnected");
            } else {
                LOGW("Frame pipe sink: writev error: %s (wrote %zd/%zu)",
                     strerror(errno), written, frame_size);
            }
            return false;
        }
        return true;
    }

    // Slow path: copy to contiguous buffer (handles linesize padding)
    // Allocate/reuse buffer
    if (!fps->frame_buf || fps->frame_buf_size < frame_size) {
        free(fps->frame_buf);
        fps->frame_buf = malloc(frame_size);
        if (!fps->frame_buf) {
            LOGE("Frame pipe sink: failed to allocate frame buffer");
            return false;
        }
        fps->frame_buf_size = frame_size;
    }

    uint8_t *dst = fps->frame_buf;

    // Copy Y plane
    for (int y = 0; y < height; y++) {
        memcpy(dst, frame->data[0] + y * frame->linesize[0], width);
        dst += width;
    }
    // Copy U plane
    for (int y = 0; y < uv_height; y++) {
        memcpy(dst, frame->data[1] + y * frame->linesize[1], uv_width);
        dst += uv_width;
    }
    // Copy V plane
    for (int y = 0; y < uv_height; y++) {
        memcpy(dst, frame->data[2] + y * frame->linesize[2], uv_width);
        dst += uv_width;
    }

    // Single write for entire frame
    ssize_t written = write(fps->fd, fps->frame_buf, frame_size);
    if (written != (ssize_t)frame_size) {
        if (errno == EPIPE) {
            LOGD("Frame pipe sink: reader disconnected");
        } else {
            LOGW("Frame pipe sink: write error: %s (wrote %zd/%zu)",
                 strerror(errno), written, frame_size);
        }
        return false;
    }

    return true;
}

static bool
sc_frame_pipe_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx) {
    struct sc_frame_pipe_sink *fps = DOWNCAST(sink);

    // Store time_base for PTS conversion
    fps->time_base = ctx->time_base;

    // Create the named pipe (FIFO)
    unlink(fps->pipe_path);
    if (mkfifo(fps->pipe_path, 0660) < 0) {
        LOGE("Frame pipe sink: failed to create FIFO %s: %s",
             fps->pipe_path, strerror(errno));
        return false;
    }

    LOGI("Frame pipe sink: created FIFO %s, waiting for reader...", fps->pipe_path);

    // Open the pipe for writing (this blocks until a reader connects)
    fps->fd = open(fps->pipe_path, O_WRONLY);
    if (fps->fd < 0) {
        LOGE("Frame pipe sink: failed to open FIFO: %s", strerror(errno));
        unlink(fps->pipe_path);
        return false;
    }

    fps->width = 0;
    fps->height = 0;
    fps->stopped = false;

    LOGI("Frame pipe sink: reader connected, streaming YUV420P frames with PTS");
    return true;
}

static void
sc_frame_pipe_sink_close(struct sc_frame_sink *sink) {
    struct sc_frame_pipe_sink *fps = DOWNCAST(sink);

    if (fps->fd >= 0) {
        close(fps->fd);
        fps->fd = -1;
    }
    unlink(fps->pipe_path);

    LOGI("Frame pipe sink: closed");
}

static bool
sc_frame_pipe_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_frame_pipe_sink *fps = DOWNCAST(sink);

    if (fps->fd < 0 || fps->stopped) {
        return false;
    }

    // Check for dimension change
    if (frame->width != fps->width || frame->height != fps->height) {
        if (!write_header(fps, frame->width, frame->height)) {
            fps->stopped = true;
            return false;
        }
    }

    // Convert PTS to microseconds
    int64_t pts_us = 0;
    if (frame->pts != AV_NOPTS_VALUE) {
        // pts_us = frame->pts * time_base * 1000000
        pts_us = av_rescale_q(frame->pts, fps->time_base, (AVRational){1, 1000000});
    }

    // Write PTS before frame data
    if (!write_pts(fps, pts_us)) {
        fps->stopped = true;
        return false;
    }

    // Write raw YUV frame data
    if (!write_frame_data(fps, frame)) {
        fps->stopped = true;
        return false;
    }

    return true;
}

bool
sc_frame_pipe_sink_init(struct sc_frame_pipe_sink *fps, const char *pipe_path) {
    fps->pipe_path = strdup(pipe_path);
    if (!fps->pipe_path) {
        return false;
    }

    fps->fd = -1;
    fps->width = 0;
    fps->height = 0;
    fps->stopped = false;
    fps->frame_buf = NULL;
    fps->frame_buf_size = 0;

    static const struct sc_frame_sink_ops ops = {
        .open = sc_frame_pipe_sink_open,
        .close = sc_frame_pipe_sink_close,
        .push = sc_frame_pipe_sink_push,
    };

    fps->frame_sink.ops = &ops;

    return true;
}

void
sc_frame_pipe_sink_destroy(struct sc_frame_pipe_sink *fps) {
    free(fps->frame_buf);
    free(fps->pipe_path);
}
