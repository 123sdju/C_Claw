#include "cc_board_tools_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct board_mmap_buffer {
    void *start;
    size_t length;
} board_mmap_buffer_t;

typedef struct camera_preview_state {
    pthread_mutex_t lock;
    pthread_mutex_t io_lock;
    pthread_t thread;
    int initialized;
    int running;
    int stop_requested;
    char device[128];
    char fb[128];
    int width;
    int height;
    int interval_ms;
    unsigned long frames_displayed;
    unsigned long errors;
} camera_preview_state_t;

static camera_preview_state_t g_preview;

static void preview_state_init(void)
{
    if (g_preview.initialized) return;
    pthread_mutex_init(&g_preview.lock, NULL);
    pthread_mutex_init(&g_preview.io_lock, NULL);
    g_preview.initialized = 1;
}

static int write_bmp_rgb565(const char *path, const uint16_t *pixels, int width, int height)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row_bytes = width * 3;
    int padded = (row_bytes + 3) & ~3;
    int data_size = padded * height;
    int file_size = 54 + data_size;
    unsigned char header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char)(file_size);
    header[3] = (unsigned char)(file_size >> 8);
    header[4] = (unsigned char)(file_size >> 16);
    header[5] = (unsigned char)(file_size >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char)(width);
    header[19] = (unsigned char)(width >> 8);
    header[20] = (unsigned char)(width >> 16);
    header[21] = (unsigned char)(width >> 24);
    header[22] = (unsigned char)(height);
    header[23] = (unsigned char)(height >> 8);
    header[24] = (unsigned char)(height >> 16);
    header[25] = (unsigned char)(height >> 24);
    header[26] = 1;
    header[28] = 24;
    header[34] = (unsigned char)(data_size);
    header[35] = (unsigned char)(data_size >> 8);
    header[36] = (unsigned char)(data_size >> 16);
    header[37] = (unsigned char)(data_size >> 24);
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }
    unsigned char *row = calloc(1, (size_t)padded);
    if (!row) {
        fclose(f);
        return -1;
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            uint16_t p = pixels[y * width + x];
            unsigned char r = (unsigned char)(((p >> 11) & 0x1f) * 255 / 31);
            unsigned char g = (unsigned char)(((p >> 5) & 0x3f) * 255 / 63);
            unsigned char b = (unsigned char)((p & 0x1f) * 255 / 31);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        if (fwrite(row, 1, (size_t)padded, f) != (size_t)padded) {
            free(row);
            fclose(f);
            return -1;
        }
    }
    free(row);
    fclose(f);
    return 0;
}

static int display_rgb565_to_fb(const char *fb_path, const uint16_t *pixels, int width, int height)
{
    int fd = open(fb_path ? fb_path : CC_BOARD_DEFAULT_FB_DEVICE, O_RDWR);
    if (fd < 0) return -1;
    struct fb_var_screeninfo var_info;
    struct fb_fix_screeninfo fix_info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var_info) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix_info) < 0) {
        close(fd);
        return -1;
    }
    size_t map_len = (size_t)fix_info.line_length * var_info.yres;
    unsigned char *fb = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        close(fd);
        return -1;
    }
    int copy_w = width < (int)var_info.xres ? width : (int)var_info.xres;
    int copy_h = height < (int)var_info.yres ? height : (int)var_info.yres;
    for (int y = 0; y < copy_h; ++y) {
        unsigned char *dst = fb + (size_t)y * fix_info.line_length;
        const uint16_t *src = pixels + (size_t)y * width;
        if (var_info.bits_per_pixel == 16) {
            memcpy(dst, src, (size_t)copy_w * 2);
        } else if (var_info.bits_per_pixel == 32) {
            uint32_t *d32 = (uint32_t *)dst;
            for (int x = 0; x < copy_w; ++x) {
                uint16_t p = src[x];
                uint32_t r = ((p >> 11) & 0x1f) * 255 / 31;
                uint32_t g = ((p >> 5) & 0x3f) * 255 / 63;
                uint32_t b = (p & 0x1f) * 255 / 31;
                d32[x] = (r << 16) | (g << 8) | b;
            }
        }
    }
    munmap(fb, map_len);
    close(fd);
    return 0;
}

static int capture_rgb565_frame_unlocked(
    const char *device,
    int width,
    int height,
    uint16_t **out_pixels,
    int *out_width,
    int *out_height
)
{
    int fd = open(device ? device : CC_BOARD_DEFAULT_CAMERA_DEVICE, O_RDWR);
    if (fd < 0) return -1;

    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (unsigned int)width;
    fmt.fmt.pix.height = (unsigned int)height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0 ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        close(fd);
        return -1;
    }
    width = (int)fmt.fmt.pix.width;
    height = (int)fmt.fmt.pix.height;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 3;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        close(fd);
        return -1;
    }

    board_mmap_buffer_t buffers[4];
    memset(buffers, 0, sizeof(buffers));
    unsigned int buffer_count = req.count > 4 ? 4 : req.count;
    for (unsigned int i = 0; i < buffer_count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) goto fail;
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) goto fail;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) goto fail;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) goto fail;

    uint16_t *pixels = malloc((size_t)width * height * 2);
    if (!pixels) goto stream_fail;

    int got_frame = 0;
    for (int attempt = 0; attempt < 10 && !got_frame; ++attempt) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) continue;
        size_t expected = (size_t)width * height * 2;
        if (buf.index < buffer_count && buf.bytesused >= expected) {
            memcpy(pixels, buffers[buf.index].start, expected);
            got_frame = 1;
        }
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned int i = 0; i < buffer_count; ++i) {
        if (buffers[i].start && buffers[i].start != MAP_FAILED) {
            munmap(buffers[i].start, buffers[i].length);
        }
    }
    close(fd);
    if (!got_frame) {
        free(pixels);
        return -1;
    }
    *out_pixels = pixels;
    *out_width = width;
    *out_height = height;
    return 0;

stream_fail:
    ioctl(fd, VIDIOC_STREAMOFF, &type);
fail:
    for (unsigned int i = 0; i < buffer_count; ++i) {
        if (buffers[i].start && buffers[i].start != MAP_FAILED) {
            munmap(buffers[i].start, buffers[i].length);
        }
    }
    close(fd);
    return -1;
}

static int capture_rgb565_frame(const char *device, int width, int height, uint16_t **out_pixels, int *out_width, int *out_height)
{
    preview_state_init();
    pthread_mutex_lock(&g_preview.io_lock);
    int rc = capture_rgb565_frame_unlocked(device, width, height, out_pixels, out_width, out_height);
    pthread_mutex_unlock(&g_preview.io_lock);
    return rc;
}

static void sleep_preview_interval(int interval_ms)
{
    if (interval_ms < 10) interval_ms = 10;
    struct timespec ts;
    ts.tv_sec = interval_ms / 1000;
    ts.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/*
 * The preview loop intentionally serializes V4L2 access with still captures.
 * It favors robustness over maximum FPS: if a foreground capture runs, preview
 * waits instead of racing for /dev/video0 and leaving the driver in a bad state.
 */
static void *preview_thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        char device[128];
        char fb[128];
        int width;
        int height;
        int interval_ms;

        pthread_mutex_lock(&g_preview.lock);
        int should_stop = g_preview.stop_requested;
        snprintf(device, sizeof(device), "%s", g_preview.device);
        snprintf(fb, sizeof(fb), "%s", g_preview.fb);
        width = g_preview.width;
        height = g_preview.height;
        interval_ms = g_preview.interval_ms;
        pthread_mutex_unlock(&g_preview.lock);
        if (should_stop) break;

        uint16_t *pixels = NULL;
        int actual_w = 0;
        int actual_h = 0;
        pthread_mutex_lock(&g_preview.io_lock);
        int ok = capture_rgb565_frame_unlocked(device, width, height, &pixels, &actual_w, &actual_h);
        pthread_mutex_unlock(&g_preview.io_lock);
        if (ok == 0) {
            if (display_rgb565_to_fb(fb, pixels, actual_w, actual_h) == 0) {
                pthread_mutex_lock(&g_preview.lock);
                g_preview.frames_displayed++;
                pthread_mutex_unlock(&g_preview.lock);
            } else {
                pthread_mutex_lock(&g_preview.lock);
                g_preview.errors++;
                pthread_mutex_unlock(&g_preview.lock);
            }
            free(pixels);
        } else {
            pthread_mutex_lock(&g_preview.lock);
            g_preview.errors++;
            pthread_mutex_unlock(&g_preview.lock);
        }
        sleep_preview_interval(interval_ms);
    }

    pthread_mutex_lock(&g_preview.lock);
    g_preview.running = 0;
    g_preview.stop_requested = 0;
    pthread_mutex_unlock(&g_preview.lock);
    return NULL;
}

static void preview_stop_blocking(void)
{
    preview_state_init();
    pthread_mutex_lock(&g_preview.lock);
    int was_running = g_preview.running;
    pthread_t thread = g_preview.thread;
    if (was_running) g_preview.stop_requested = 1;
    pthread_mutex_unlock(&g_preview.lock);
    if (was_running) pthread_join(thread, NULL);
}

static void set_preview_status_json(cc_tool_result_t *out_result, const char *operation)
{
    preview_state_init();
    pthread_mutex_lock(&g_preview.lock);
    cc_json_value_t *content = cc_json_create_object();
    cc_json_object_set(content, "ok", cc_json_create_bool(1));
    cc_json_object_set(content, "operation", cc_json_create_string(operation));
    cc_json_object_set(content, "running", cc_json_create_bool(g_preview.running));
    cc_json_object_set(content, "device", cc_json_create_string(g_preview.device));
    cc_json_object_set(content, "fb", cc_json_create_string(g_preview.fb));
    cc_json_object_set(content, "width", cc_json_create_number(g_preview.width));
    cc_json_object_set(content, "height", cc_json_create_number(g_preview.height));
    cc_json_object_set(content, "preview_interval_ms", cc_json_create_number(g_preview.interval_ms));
    cc_json_object_set(content, "frames_displayed", cc_json_create_number((double)g_preview.frames_displayed));
    cc_json_object_set(content, "errors", cc_json_create_number((double)g_preview.errors));
    pthread_mutex_unlock(&g_preview.lock);
    cc_board_set_success_json(out_result, cc_json_stringify_unformatted(content));
    cc_json_destroy(content);
}

static cc_result_t camera_preview_start(const cc_json_value_t *args, cc_tool_result_t *out_result)
{
    preview_state_init();
    const char *device = cc_board_json_string_or(args, "device", CC_BOARD_DEFAULT_CAMERA_DEVICE);
    const char *fb = cc_board_json_string_or(args, "fb", CC_BOARD_DEFAULT_FB_DEVICE);
    int width = cc_board_json_int_or(args, "width", 640);
    int height = cc_board_json_int_or(args, "height", 480);
    int interval_ms = cc_board_json_int_or(args, "preview_interval_ms", 100);
    if (width <= 0 || height <= 0 || interval_ms <= 0) {
        cc_board_set_error(out_result, "Invalid camera preview parameters");
        return cc_result_ok();
    }

    pthread_mutex_lock(&g_preview.lock);
    if (g_preview.running) {
        pthread_mutex_unlock(&g_preview.lock);
        set_preview_status_json(out_result, "preview_start");
        return cc_result_ok();
    }
    snprintf(g_preview.device, sizeof(g_preview.device), "%s", device);
    snprintf(g_preview.fb, sizeof(g_preview.fb), "%s", fb);
    g_preview.width = width;
    g_preview.height = height;
    g_preview.interval_ms = interval_ms;
    g_preview.frames_displayed = 0;
    g_preview.errors = 0;
    g_preview.stop_requested = 0;
    g_preview.running = 1;
    int rc = pthread_create(&g_preview.thread, NULL, preview_thread_main, NULL);
    if (rc != 0) {
        g_preview.running = 0;
        pthread_mutex_unlock(&g_preview.lock);
        cc_board_set_error(out_result, "Failed to start camera preview thread");
        return cc_result_ok();
    }
    pthread_mutex_unlock(&g_preview.lock);
    set_preview_status_json(out_result, "preview_start");
    return cc_result_ok();
}

static cc_result_t camera_capture(const cc_json_value_t *args, const cc_tool_context_t *ctx, cc_tool_result_t *out_result)
{
    const char *device = cc_board_json_string_or(args, "device", CC_BOARD_DEFAULT_CAMERA_DEVICE);
    const char *fb = cc_board_json_string_or(args, "fb", CC_BOARD_DEFAULT_FB_DEVICE);
    int width = cc_board_json_int_or(args, "width", 640);
    int height = cc_board_json_int_or(args, "height", 480);
    int display = cc_board_json_bool_or(args, "display", 1);
    int embed_base64 = cc_board_json_bool_or(args, "embed_base64", 1);
    char *path = cc_board_output_path(ctx, args, "output_path", "camera", "bmp");
    if (!path) {
        cc_board_set_error(out_result, "Failed to create board media output path");
        return cc_result_ok();
    }

    uint16_t *pixels = NULL;
    int actual_w = 0;
    int actual_h = 0;
    if (capture_rgb565_frame(device, width, height, &pixels, &actual_w, &actual_h) != 0) {
        free(path);
        cc_board_set_error(out_result, "Failed to capture RGB565 frame from V4L2 camera");
        return cc_result_ok();
    }
    if (write_bmp_rgb565(path, pixels, actual_w, actual_h) != 0) {
        free(pixels);
        free(path);
        cc_board_set_error(out_result, "Failed to write captured BMP image");
        return cc_result_ok();
    }
    int display_synced = display ? (display_rgb565_to_fb(fb, pixels, actual_w, actual_h) == 0) : 0;
    free(pixels);

    size_t bytes = 0;
    char *b64 = cc_board_file_base64_if_requested(path, embed_base64, &bytes);
    char *id = cc_board_now_id("camera");
    char *artifacts = cc_board_artifact_json(id, "image", "image/bmp", path, b64, bytes, actual_w, actual_h, 0);

    cc_json_value_t *content = cc_json_create_object();
    cc_json_object_set(content, "ok", cc_json_create_bool(1));
    cc_json_object_set(content, "operation", cc_json_create_string("capture"));
    cc_json_object_set(content, "path", cc_json_create_string(path));
    cc_json_object_set(content, "mime", cc_json_create_string("image/bmp"));
    cc_json_object_set(content, "bytes", cc_json_create_number((double)bytes));
    cc_json_object_set(content, "width", cc_json_create_number(actual_w));
    cc_json_object_set(content, "height", cc_json_create_number(actual_h));
    cc_json_object_set(content, "display_synced", cc_json_create_bool(display_synced));
    char *content_json = cc_json_stringify_unformatted(content);
    cc_json_destroy(content);
    cc_board_set_success_json(out_result, content_json);
    out_result->artifacts_json = artifacts;

    free(id);
    free(b64);
    free(path);
    return cc_result_ok();
}

static cc_result_t board_camera_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)self;
    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json && *args_json ? args_json : "{}", &args);
    if (rc.code != CC_OK) {
        cc_board_set_error(out_result, "Invalid camera arguments JSON");
        return cc_result_ok();
    }
    const char *operation = cc_board_json_string_or(args, "operation", "capture");
    if (strcmp(operation, "preview_start") == 0) {
        rc = camera_preview_start(args, out_result);
    } else if (strcmp(operation, "preview_stop") == 0) {
        preview_stop_blocking();
        set_preview_status_json(out_result, "preview_stop");
        rc = cc_result_ok();
    } else if (strcmp(operation, "preview_status") == 0) {
        set_preview_status_json(out_result, "preview_status");
        rc = cc_result_ok();
    } else if (strcmp(operation, "capture") == 0) {
        rc = camera_capture(args, ctx, out_result);
    } else {
        cc_board_set_error(out_result, "Invalid camera operation; use capture, preview_start, preview_stop, or preview_status");
        rc = cc_result_ok();
    }
    cc_json_destroy(args);
    return rc;
}

const cc_board_tool_ops_t cc_board_camera_tool_ops = {
    "board.camera",
    "Capture a V4L2 RGB565 frame, save it as BMP, return an image artifact, or run a background preview that continuously displays camera frames on /dev/fb0.",
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":[\"capture\",\"preview_start\",\"preview_stop\",\"preview_status\"]},\"device\":{\"type\":\"string\"},\"fb\":{\"type\":\"string\"},\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},\"output_path\":{\"type\":\"string\"},\"display\":{\"type\":\"boolean\"},\"embed_base64\":{\"type\":\"boolean\"},\"preview_interval_ms\":{\"type\":\"integer\",\"minimum\":10}}}",
    board_camera_call,
    preview_stop_blocking
};
