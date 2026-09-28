#include "capture_adapter.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>

/* 在之前的 hook.c 中实现。 */
extern int hook_publish_bgrx(
    const void *src,
    size_t available,
    int width,
    int height,
    size_t stride
);

extern void hook_clear_frame(void);

struct pw_capture {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;

    struct spa_hook core_listener;
    struct spa_hook stream_listener;

    bool have_core_listener;
    bool have_stream_listener;
    bool loop_started;
    bool format_valid;

    struct spa_video_info_raw format;
    uint64_t debug_process_count;
};

static pthread_once_t pw_init_once = PTHREAD_ONCE_INIT;

static void init_pipewire(void)
{
    pw_init(NULL, NULL);
}

static void on_core_error(
    void *userdata,
    uint32_t id,
    int seq,
    int res,
    const char *message)
{
    (void)userdata;
    (void)seq;

    fprintf(stderr,
            "[pw-hook] core error: id=%" PRIu32
            ", res=%d, %s\n",
            id, res, message ? message : "");

    hook_clear_frame();
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static void on_state_changed(
    void *userdata,
    enum pw_stream_state old,
    enum pw_stream_state state,
    const char *error)
{
    (void)userdata;
    (void)old;

    fprintf(stderr, "[pw-hook] stream: %s%s%s\n",
            pw_stream_state_as_string(state),
            error ? ": " : "",
            error ? error : "");

    if (state == PW_STREAM_STATE_ERROR ||
        state == PW_STREAM_STATE_UNCONNECTED)
        hook_clear_frame();
}

static void on_param_changed(
    void *userdata,
    uint32_t id,
    const struct spa_pod *param)
{
    struct pw_capture *c = userdata;

    if (id != SPA_PARAM_Format)
        return;

    c->format_valid = false;
    hook_clear_frame();

    if (!param)
        return;

    uint32_t media_type;
    uint32_t media_subtype;

    if (spa_format_parse(
            param, &media_type, &media_subtype) < 0)
        return;

    if (media_type != SPA_MEDIA_TYPE_video ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    struct spa_video_info_raw format = {0};

    if (spa_format_video_raw_parse(param, &format) < 0)
        return;

    if (format.format != SPA_VIDEO_FORMAT_BGRx ||
        format.size.width == 0 ||
        format.size.height == 0 ||
        format.size.width > INT_MAX ||
        format.size.height > INT_MAX) {
        fprintf(stderr, "[pw-hook] unsupported video format\n");
        return;
    }

    c->format = format;
    c->format_valid = true;

    fprintf(stderr, "[pw-hook] BGRx %" PRIu32 " x %" PRIu32 "\n",
            format.size.width, format.size.height);

    /*
     * 本实现需要 CPU 可以直接访问的 buffer。
     * 不请求 DMA-BUF。
     */
    uint8_t storage[256];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(storage, sizeof(storage));

    const struct spa_pod *params[1];

    params[0] = spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_ParamBuffers,
        SPA_PARAM_Buffers,

        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int(
            (1 << SPA_DATA_MemPtr) |
            (1 << SPA_DATA_MemFd)
        )
    );

    if (!params[0]) {
        c->format_valid = false;
        return;
    }

    int result = pw_stream_update_params(c->stream, params, 1);

    if (result < 0) {
        fprintf(stderr,
                "[pw-hook] update buffer params: %s\n",
                strerror(-result));
        c->format_valid = false;
    }
}

static void on_process(void *userdata)
{
    struct pw_capture *c = userdata;

    uint64_t n = ++c->debug_process_count;
    bool log_this = n <= 5 || n % 120 == 0;

    if (log_this) {
        fprintf(stderr,
                "[pw pid=%ld] process #%" PRIu64
                " format_valid=%d\n",
                (long)getpid(),
                n,
                (int)c->format_valid);
    }

    struct pw_buffer *pwbuf =
        pw_stream_dequeue_buffer(c->stream);

    if (!pwbuf) {
        if (log_this)
            fprintf(stderr, "[pw] no dequeued buffer\n");

        return;
    }

    struct spa_buffer *buffer = pwbuf->buffer;

    if (!c->format_valid || !buffer || buffer->n_datas != 1) {
        if (log_this) {
            fprintf(stderr,
                    "[pw] rejected: valid=%d buffer=%p "
                    "n_datas=%u\n",
                    (int)c->format_valid,
                    (void *)buffer,
                    buffer ? buffer->n_datas : 0);
        }

        goto done;
    }

    struct spa_data *data = &buffer->datas[0];
    struct spa_chunk *chunk = data->chunk;

    if (log_this) {
        fprintf(stderr,
                "[pw] type=%u data=%p maxsize=%u "
                "chunk=%p\n",
                data->type,
                data->data,
                data->maxsize,
                (void *)chunk);

        if (chunk) {
            fprintf(stderr,
                    "[pw] offset=%u size=%u stride=%d "
                    "flags=0x%x\n",
                    chunk->offset,
                    chunk->size,
                    chunk->stride,
                    (unsigned)chunk->flags);
        }
    }

    if (!data->data || !chunk ||
        (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
        chunk->stride <= 0 ||
        chunk->offset > data->maxsize ||
        chunk->size > data->maxsize - chunk->offset) {
        if (log_this)
            fprintf(stderr, "[pw] rejected buffer layout\n");

        goto done;
    }

    int result = hook_publish_bgrx(
        (const uint8_t *)data->data + chunk->offset,
        chunk->size,
        (int)c->format.size.width,
        (int)c->format.size.height,
        (size_t)chunk->stride
    );

    if (log_this) {
        fprintf(stderr,
                "[pw] publish=%d src=%ux%u\n",
                result,
                c->format.size.width,
                c->format.size.height);
    }

done:
    pw_stream_queue_buffer(c->stream, pwbuf);
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

void pw_capture_stop(struct pw_capture *c)
{
    if (!c)
        return;

    /*
     * 本实现不使用 RT_PROCESS。
     * 先停止线程，之后不会再执行这些接收回调。
     *
     * 不要持有 loop lock 调用 stop，
     * 也不要从该 loop 自己的回调中调用 stop。
     */
    if (c->loop_started) {
        pw_thread_loop_stop(c->loop);
        c->loop_started = false;
    }

    if (c->have_stream_listener)
        spa_hook_remove(&c->stream_listener);

    if (c->stream)
        pw_stream_destroy(c->stream);

    if (c->have_core_listener)
        spa_hook_remove(&c->core_listener);

    if (c->core)
        pw_core_disconnect(c->core);

    if (c->context)
        pw_context_destroy(c->context);

    if (c->loop)
        pw_thread_loop_destroy(c->loop);

    hook_clear_frame();
    free(c);
}

struct pw_capture *pw_capture_start(
    int pipewire_fd,
    const struct portal_stream *source)
{
    if (pipewire_fd < 0 || !source) {
        if (pipewire_fd >= 0)
            close(pipewire_fd);

        errno = EINVAL;
        return NULL;
    }

    pthread_once(&pw_init_once, init_pipewire);

    struct pw_capture *c = calloc(1, sizeof(*c));

    if (!c) {
        close(pipewire_fd);
        return NULL;
    }

    c->loop = pw_thread_loop_new("pw-hook-capture", NULL);
    if (!c->loop)
        goto fail;

    /*
     * 接收线程尚未启动，现在可以直接创建对象。
     */
    c->context = pw_context_new(
        pw_thread_loop_get_loop(c->loop),
        NULL,
        0
    );

    if (!c->context)
        goto fail;

    /*
     * connect_fd 从调用开始接管 FD，包括失败路径。
     */
    int owned_fd = pipewire_fd;
    pipewire_fd = -1;

    c->core = pw_context_connect_fd(
        c->context,
        owned_fd,
        NULL,
        0
    );

    if (!c->core)
        goto fail;

    pw_core_add_listener(
        c->core,
        &c->core_listener,
        &core_events,
        c
    );
    c->have_core_listener = true;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL
    );

    if (!props)
        goto fail;

    uint32_t target_id = source->node_id;

    if (source->has_serial) {
        char serial[32];

        snprintf(serial, sizeof(serial),
                 "%" PRIu64, source->serial);

        int result = pw_properties_set(
            props, PW_KEY_TARGET_OBJECT, serial);

        if (result < 0) {
            pw_properties_free(props);
            goto fail;
        }

        target_id = PW_ID_ANY;
    }

    /* pw_stream_new 接管 props。 */
    c->stream = pw_stream_new(
        c->core,
        "XShmGetImage replacement",
        props
    );

    if (!c->stream)
        goto fail;

    pw_stream_add_listener(
        c->stream,
        &c->stream_listener,
        &stream_events,
        c
    );
    c->have_stream_listener = true;

    /*
     * 只限制像素格式，不固定宽高。
     * 实际尺寸由 param_changed 回调获取。
     */
    uint8_t storage[512];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(storage, sizeof(storage));

    const struct spa_pod *params[1];

    params[0] = spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format,
        SPA_PARAM_EnumFormat,

        SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video),

        SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),

        SPA_FORMAT_VIDEO_format,
        SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx)
    );

    if (!params[0])
        goto fail;

    int result = pw_stream_connect(
        c->stream,
        PW_DIRECTION_INPUT,
        target_id,
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS,
        params,
        1
    );

    if (result < 0) {
        fprintf(stderr,
                "[pw-hook] stream connect: %s\n",
                strerror(-result));
        goto fail;
    }

    result = pw_thread_loop_start(c->loop);

    if (result < 0) {
        fprintf(stderr,
                "[pw-hook] thread start: %s\n",
                strerror(-result));
        goto fail;
    }

    c->loop_started = true;
    return c;

fail:
    if (pipewire_fd >= 0)
        close(pipewire_fd);

    pw_capture_stop(c);
    return NULL;
}
