// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "client-connection.h"
#include "server-bridge-api.h"
#include "xdg-toplevel-client.h"
#include "ext-foreign-toplevel-image-capture-v1.h"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>

extern void ext_capture_query_state(void *data);
extern void ext_capture_move_window(void *data);
extern void ext_capture_render_end_delta(void *data);
extern void ext_capture_wait_render(void *data);

/* The captured window: a 64x64 opaque-red main surface with a 16x16
 * opaque-green subsurface stacked at (16,16) inside the content. The capture
 * must contain the whole subtree (decoration + main surface + subsurface),
 * locked to the frame origin, while the real window moves around the screen
 * and resizes. The physical output is recorded through wlr-screencopy at the
 * same time and all frames are dumped as BMP for post-analysis. */
#define CONTENT_SIZE 64
#define SUB_SIZE 16
#define SUB_POS 16
#define MAIN_COLOR 0xffff0000u /* ARGB: opaque red */
#define SUB_COLOR 0xff00ff00u /* ARGB: opaque green */

static int g_dump_frames = 0;

struct capture_client {
    struct client_connection connection;
    struct xdg_toplevel_client toplevel;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wl_subcompositor *subcompositor;
    struct wl_surface *subsurface_surface;
    struct wl_subsurface *subsurface;
    struct wl_buffer *subsurface_buffer;

    struct ext_foreign_toplevel_list_v1 *toplevel_list;
    struct ext_foreign_toplevel_handle_v1 *toplevel_handle;
    int handle_seen;

    struct zwlr_screencopy_manager_v1 *screencopy_manager;
    struct wl_output *wl_output;

    struct ext_foreign_toplevel_image_capture_source_manager_v1 *capture_manager;
    struct ext_image_copy_capture_manager_v1 *copy_manager;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;

    uint32_t buffer_width;
    uint32_t buffer_height;
    uint32_t shm_format;
    int have_size;
    int have_shm_format;
    int session_done;
    int session_stopped;

    struct wl_buffer *target_buffer;
    void *target_data;
    size_t target_size;
    int frame_ready;
    int frame_failed;
    uint32_t frame_fail_reason;
    int frames_copied;
};

/* ------------------------------------------------------------------ */
/* Production window: 64x64 red xdg-toplevel with server-side decoration
 * negotiated before the first buffer and a green 16x16 subsurface.       */
/* ------------------------------------------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    (void)surface;
    struct xdg_toplevel_client *toplevel = data;
    toplevel->configure_serial = serial;
    toplevel->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                     struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static int wait_configured(struct capture_client *client)
{
    for (int i = 0; i < 50 && !client->toplevel.configured; i++) {
        if (wl_display_flush(client->connection.display) < 0)
            return 0;
        if (wl_display_dispatch(client->connection.display) < 0)
            return 0;
    }
    return client->toplevel.configured;
}

static struct wl_buffer *make_solid_buffer(struct wl_shm *shm, int width, int height,
                                           uint32_t argb, size_t *size_out)
{
    (void)size_out;
    const size_t size = (size_t)width * 4 * height;
    char name[64];
    snprintf(name, sizeof(name), "/ext_capture_win_%d_%d", (int)getpid(), width * height);
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return NULL;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)width * height; ++i)
        pixels[i] = argb;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    close(fd);
    if (!pool) {
        munmap(pixels, size);
        return NULL;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    return buffer;
}

static int create_mapped_window(struct capture_client *client)
{
    struct xdg_toplevel_client *toplevel = &client->toplevel;
    memset(toplevel, 0, sizeof(*toplevel));
    toplevel->compositor = client_bind(&client->connection, "wl_compositor",
                                       &wl_compositor_interface, 1);
    toplevel->wm_base = client_bind(&client->connection, "xdg_wm_base",
                                    &xdg_wm_base_interface, 1);
    toplevel->shm = client_bind(&client->connection, "wl_shm",
                                &wl_shm_interface, 1);
    if (!toplevel->compositor || !toplevel->wm_base || !toplevel->shm)
        return 0;

    xdg_wm_base_add_listener(toplevel->wm_base, &wm_base_listener, toplevel);
    toplevel->surface = wl_compositor_create_surface(toplevel->compositor);
    if (!toplevel->surface)
        return 0;
    toplevel->xdg_surface = xdg_wm_base_get_xdg_surface(toplevel->wm_base, toplevel->surface);
    if (!toplevel->xdg_surface)
        return 0;
    xdg_surface_add_listener(toplevel->xdg_surface, &xdg_surface_listener, toplevel);
    toplevel->toplevel = xdg_surface_get_toplevel(toplevel->xdg_surface);
    if (!toplevel->toplevel)
        return 0;
    xdg_toplevel_add_listener(toplevel->toplevel, &xdg_toplevel_listener, toplevel);

    /* Decoration must be created before the surface carries a buffer. */
    struct zxdg_toplevel_decoration_v1 *decoration =
        zxdg_decoration_manager_v1_get_toplevel_decoration(
            client->decoration_manager, toplevel->toplevel);
    if (!decoration)
        return 0;
    zxdg_toplevel_decoration_v1_set_mode(decoration,
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    wl_surface_commit(toplevel->surface);
    if (wl_display_roundtrip(client->connection.display) < 0 || !wait_configured(client))
        return 0;
    xdg_surface_ack_configure(toplevel->xdg_surface, toplevel->configure_serial);

    /* Map with the 64x64 opaque-red content. */
    toplevel->buffer = make_solid_buffer(toplevel->shm, CONTENT_SIZE, CONTENT_SIZE,
                                         MAIN_COLOR, NULL);
    if (!toplevel->buffer)
        return 0;
    wl_surface_attach(toplevel->surface, toplevel->buffer, 0, 0);
    wl_surface_damage(toplevel->surface, 0, 0, CONTENT_SIZE, CONTENT_SIZE);
    wl_surface_commit(toplevel->surface);
    return wl_display_roundtrip(client->connection.display) >= 0;
}

static int add_subsurface(struct capture_client *client)
{
    struct wl_shm *shm = client->toplevel.shm;
    client->subsurface_surface = wl_compositor_create_surface(client->toplevel.compositor);
    if (!client->subsurface_surface)
        return 0;
    client->subsurface = wl_subcompositor_get_subsurface(client->subcompositor,
        client->subsurface_surface, client->toplevel.surface);
    if (!client->subsurface)
        return 0;

    client->subsurface_buffer = make_solid_buffer(shm, SUB_SIZE, SUB_SIZE, SUB_COLOR, NULL);
    if (!client->subsurface_buffer)
        return 0;
    wl_surface_attach(client->subsurface_surface, client->subsurface_buffer, 0, 0);
    wl_surface_damage(client->subsurface_surface, 0, 0, SUB_SIZE, SUB_SIZE);
    wl_subsurface_set_position(client->subsurface, SUB_POS, SUB_POS);
    wl_surface_commit(client->subsurface_surface);
    wl_surface_commit(client->toplevel.surface);
    return wl_display_roundtrip(client->connection.display) >= 0;
}

/* ------------------------------------------------------------------ */
/* ext_foreign_toplevel_list                                           */
/* ------------------------------------------------------------------ */

static void toplevel_handle_title(void *data,
                                  struct ext_foreign_toplevel_handle_v1 *handle,
                                  const char *title)
{
    (void)data;
    (void)handle;
    (void)title;
}

static void toplevel_handle_app_id(void *data,
                                   struct ext_foreign_toplevel_handle_v1 *handle,
                                   const char *app_id)
{
    (void)data;
    (void)handle;
    (void)app_id;
}

static void toplevel_handle_identifier(void *data,
                                       struct ext_foreign_toplevel_handle_v1 *handle,
                                       const char *identifier)
{
    (void)data;
    (void)handle;
    (void)identifier;
}

static void toplevel_handle_done(void *data,
                                 struct ext_foreign_toplevel_handle_v1 *handle)
{
    (void)data;
    (void)handle;
}

static void toplevel_handle_closed(void *data,
                                   struct ext_foreign_toplevel_handle_v1 *handle)
{
    (void)data;
    (void)handle;
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    .title = toplevel_handle_title,
    .app_id = toplevel_handle_app_id,
    .identifier = toplevel_handle_identifier,
    .done = toplevel_handle_done,
    .closed = toplevel_handle_closed,
};

static void toplevel_list_toplevel(void *data,
                                   struct ext_foreign_toplevel_list_v1 *list,
                                   struct ext_foreign_toplevel_handle_v1 *handle)
{
    (void)list;
    struct capture_client *client = data;
    if (!client->toplevel_handle) {
        client->toplevel_handle = handle;
        ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle_listener, client);
    }
    client->handle_seen = 1;
}

static void toplevel_list_finished(void *data,
                                   struct ext_foreign_toplevel_list_v1 *list)
{
    (void)data;
    (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
    .toplevel = toplevel_list_toplevel,
    .finished = toplevel_list_finished,
};

/* ------------------------------------------------------------------ */
/* ext_image_copy_capture                                              */
/* ------------------------------------------------------------------ */

static void session_buffer_size(void *data,
                                struct ext_image_copy_capture_session_v1 *session,
                                uint32_t width, uint32_t height)
{
    (void)session;
    struct capture_client *client = data;
    client->buffer_width = width;
    client->buffer_height = height;
    client->have_size = 1;
}

static void session_shm_format(void *data,
                               struct ext_image_copy_capture_session_v1 *session,
                               uint32_t format)
{
    (void)session;
    struct capture_client *client = data;
    /* prefer ARGB8888, then anything offered */
    if (!client->have_shm_format || format == WL_SHM_FORMAT_ARGB8888)
        client->shm_format = format;
    client->have_shm_format = 1;
}

static void session_dmabuf_device(void *data,
                                  struct ext_image_copy_capture_session_v1 *session,
                                  struct wl_array *device)
{
    (void)data;
    (void)session;
    (void)device;
}

static void session_dmabuf_format(void *data,
                                  struct ext_image_copy_capture_session_v1 *session,
                                  uint32_t format, struct wl_array *modifiers)
{
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    ((struct capture_client *)data)->session_done = 1;
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    ((struct capture_client *)data)->session_stopped = 1;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

static void frame_transform(void *data,
                            struct ext_image_copy_capture_frame_v1 *frame,
                            uint32_t transform)
{
    (void)data;
    (void)frame;
    /* The capture renders the subtree unrotated. */
    if (transform != WL_OUTPUT_TRANSFORM_NORMAL)
        fprintf(stderr, "unexpected capture transform %u\n", transform);
}

static void frame_damage(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void frame_presentation_time(void *data,
                                    struct ext_image_copy_capture_frame_v1 *frame,
                                    uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                                    uint32_t tv_nsec)
{
    (void)data;
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
    (void)frame;
    struct capture_client *client = data;
    client->frame_ready = 1;
    client->frames_copied++;
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t reason)
{
    (void)frame;
    struct capture_client *client = data;
    client->frame_failed = 1;
    client->frame_fail_reason = reason;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static int create_target_buffer(struct capture_client *client)
{
    const size_t size = (size_t)client->buffer_width * 4 * client->buffer_height;
    char name[64];
    snprintf(name, sizeof(name), "/ext_capture_target_%d_%u", (int)getpid(),
             (unsigned)client->buffer_width * (unsigned)client->buffer_height);
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return 0;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return 0;
    }
    client->target_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (client->target_data == MAP_FAILED) {
        client->target_data = NULL;
        close(fd);
        return 0;
    }
    memset(client->target_data, 0, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(client->toplevel.shm, fd, (int)size);
    close(fd);
    if (!pool)
        return 0;
    client->target_buffer = wl_shm_pool_create_buffer(pool,
                                                       0,
                                                       (int)client->buffer_width,
                                                       (int)client->buffer_height,
                                                       (int)client->buffer_width * 4,
                                                       client->shm_format);
    wl_shm_pool_destroy(pool);
    if (!client->target_buffer)
        return 0;
    client->target_size = size;
    return 1;
}

static void release_target_buffer(struct capture_client *client)
{
    if (client->target_buffer) {
        wl_buffer_destroy(client->target_buffer);
        client->target_buffer = NULL;
    }
    if (client->target_data) {
        munmap(client->target_data, client->target_size);
        client->target_data = NULL;
    }
    client->have_size = 0;
}

static int wait_session_done(struct capture_client *client)
{
    for (int i = 0; i < 100 && !client->session_done; i++) {
        if (wl_display_dispatch(client->connection.display) < 0)
            return 0;
    }
    for (int i = 0; i < 100 && !client->have_size; i++) {
        if (wl_display_dispatch(client->connection.display) < 0)
            return 0;
    }
    return client->session_done && client->have_size && client->have_shm_format;
}

/* Deadline helper for event waits: returns current CLOCK_MONOTONIC time in
 * milliseconds. Waiting is poll-based with an absolute deadline so slow
 * software-rendering environments (CI containers, llvmpipe) are not bounded
 * by roundtrip speed — a single composited frame there can take hundreds of
 * milliseconds while roundtrips complete in microseconds. */
static int64_t wait_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Pump already-buffered wayland events without blocking; returns the number
 * of events dispatched, or -1 on connection error. */
static int pump_pending(struct wl_display *display)
{
    int dispatched = 0;
    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0)
            return -1;
        dispatched++;
    }
    /* Nothing buffered; release the read lock. The caller polls the fd and
     * dispatches when it becomes readable. */
    wl_display_cancel_read(display);
    return dispatched;
}

/* Wait (poll + dispatch) until any of the two flag variables becomes
 * non-zero or the deadline in ms elapses. Returns 1 when a flag was set,
 * 0 on timeout, -1 on error. */
static int wait_flag_on_socket(struct wl_display *display, const volatile int *flag1,
                               const volatile int *flag2, int timeout_ms)
{
    const int64_t deadline = wait_now_ms() + timeout_ms;
    while (!*flag1 && !*flag2) {
        if (pump_pending(display) < 0)
            return -1;
        if (*flag1 || *flag2)
            return 1;
        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        const int pret = poll(&pfd, 1, 100);
        if (pret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pret > 0 && wl_display_dispatch(display) < 0)
            return -1;
        if (wait_now_ms() > deadline)
            return 0;
    }
    return 1;
}

/* Returns 1 when one capture frame completes with a ready event. The snapshot
 * is produced inside natural compositor frames; the wait is deadline-based
 * (see wait_flag_on_socket) so slow CI environments are handled. */
static int capture_one_frame(struct capture_client *client)
{
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(client->session);
    if (!frame)
        return 0;
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, client);

    client->frame_ready = 0;
    client->frame_failed = 0;

    ext_image_copy_capture_frame_v1_attach_buffer(frame, client->target_buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0,
        (int32_t)client->buffer_width, (int32_t)client->buffer_height);
    ext_image_copy_capture_frame_v1_capture(frame);
    wl_display_flush(client->connection.display);

    const int wret = wait_flag_on_socket(client->connection.display,
                                         &client->frame_ready, &client->frame_failed,
                                         5000);
    ext_image_copy_capture_frame_v1_destroy(frame);
    if (!client->frame_ready) {
        fprintf(stderr, "capture frame not ready within deadline (failed=%d reason=%u wret=%d)\n",
                client->frame_failed, client->frame_fail_reason, wret);
        return 0;
    }
    return 1;
}

/* pixel classification shared by capture-frame and output-frame checks */
static int pixel_is_red(const struct capture_client *client, uint32_t pixel)
{
    switch (client->shm_format) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888:
        return ((pixel >> 16) & 0xff) > 200 && ((pixel >> 8) & 0xff) < 60
            && (pixel & 0xff) < 60;
    case WL_SHM_FORMAT_ABGR8888:
    case WL_SHM_FORMAT_XBGR8888:
        return (pixel & 0xff) > 200 && ((pixel >> 8) & 0xff) < 60
            && ((pixel >> 16) & 0xff) < 60;
    default:
        return 0;
    }
}

static int pixel_is_green(const struct capture_client *client, uint32_t pixel)
{
    switch (client->shm_format) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888:
        return ((pixel >> 8) & 0xff) > 200 && ((pixel >> 16) & 0xff) < 60
            && (pixel & 0xff) < 60;
    case WL_SHM_FORMAT_ABGR8888:
    case WL_SHM_FORMAT_XBGR8888:
        return ((pixel >> 8) & 0xff) > 200 && (pixel & 0xff) < 60
            && ((pixel >> 16) & 0xff) < 60;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* main-render verification: screencopy the output and locate the      */
/* captured window on screen while the capture session is active.      */
/* Output frames are additionally dumped as BMP files under           */
/* $EXT_CAPTURE_DUMP_DIR for post-analysis.                           */
/* ------------------------------------------------------------------ */

struct output_frame {
    void *data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct screencopy_state {
    struct output_frame frame;
    int ready;
    int failed;
    int buffer_seen;
};

static void sc_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                      uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    (void)frame;
    struct screencopy_state *sc = data;
    sc->frame.format = format;
    sc->frame.width = width;
    sc->frame.height = height;
    sc->frame.stride = stride;
    sc->buffer_seen++;
}

static void sc_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
    (void)data;
    (void)frame;
    (void)flags;
}

static void sc_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                      uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void sc_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                            uint32_t format, uint32_t width, uint32_t height)
{
    (void)data;
    (void)frame;
    (void)format;
    (void)width;
    (void)height;
}

static void sc_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                     uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
    ((struct screencopy_state *)data)->ready = 1;
}

static void sc_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    (void)frame;
    ((struct screencopy_state *)data)->failed = 1;
}

static const struct zwlr_screencopy_frame_v1_listener sc_listener = {
    .buffer = sc_buffer,
    .flags = sc_flags,
    .ready = sc_ready,
    .failed = sc_failed,
    .damage = sc_damage,
    .linux_dmabuf = sc_linux_dmabuf,
};

static void free_output_frame(struct output_frame *f)
{
    if (f->data)
        munmap(f->data, (size_t)f->stride * f->height);
    memset(f, 0, sizeof(*f));
}

static void write_bmp(const char *path, const struct output_frame *f)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "cannot open %s for dump\n", path);
        return;
    }
    const int32_t w = (int32_t)f->width;
    const int32_t h = (int32_t)f->height;
    const uint32_t data_size = f->stride * f->height;
    const uint32_t off = 14 + 40;
    const uint32_t file_size = off + data_size;
    unsigned char header[14] = { 0 };
    unsigned char dib[40] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    memcpy(header + 2, &file_size, 4);
    memcpy(header + 10, &off, 4);
    memcpy(dib, &dib[0], 0);
    const int32_t dib_size = 40;
    const int16_t planes = 1, bpp = 32;
    const int32_t half = -h; /* top-down */
    memcpy(dib, &dib_size, 4);
    memcpy(dib + 4, &w, 4);
    memcpy(dib + 8, &half, 4);
    memcpy(dib + 12, &planes, 2);
    memcpy(dib + 14, &bpp, 2);
    memcpy(dib + 20, &data_size, 4);
    fwrite(header, 1, sizeof(header), fp);
    fwrite(dib, 1, sizeof(dib), fp);
    for (int32_t y = 0; y < h; y++) {
        const char *line = (const char *)f->data + (size_t)y * f->stride;
        for (int32_t x = 0; x < w; x++) {
            /* BGRA file order; source shm formats are ARGB/XRGB (B,X,R,X in
             * memory) and ABGR/XBGR (R,B,G,X in memory). */
            unsigned char bgra[4];
            uint32_t px;
            memcpy(&px, line + (size_t)x * 4, 4);
            const unsigned char a = (px >> 24) & 0xff;
            const unsigned char r = (px >> 16) & 0xff;
            const unsigned char g = (px >> 8) & 0xff;
            const unsigned char b = px & 0xff;
            bgra[0] = b; bgra[1] = g; bgra[2] = r; bgra[3] = a;
            fwrite(bgra, 1, 4, fp);
        }
    }
    fclose(fp);
}

static void dump_output_frame(int index, const struct output_frame *f)
{
    if (!g_dump_frames || !f->data)
        return;
    const char *dir = getenv("EXT_CAPTURE_DUMP_DIR");
    if (!dir || !dir[0])
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/output-%03d.bmp", dir, index);
    write_bmp(path, f);
    fprintf(stderr, "dumped %s (%ux%u)\n", path, f->width, f->height);
}

/* Copy the whole output through wlr-screencopy and return its pixels. The
 * copy waits for the natural frame loop; a bounded dispatch budget fails
 * loudly instead of hanging. */
static int capture_output_frame(struct capture_client *client, struct output_frame *out)
{
    struct screencopy_state sc = { 0 };
    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(client->screencopy_manager, 0,
                                                  client->wl_output);
    if (!frame)
        return 0;
    memset(out, 0, sizeof(*out));
    zwlr_screencopy_frame_v1_add_listener(frame, &sc_listener, &sc);
    if (wl_display_roundtrip(client->connection.display) < 0)
        goto fail;
    if (!sc.buffer_seen) {
        fprintf(stderr, "output screencopy: no buffer event (output disabled?)\n");
        goto fail;
    }
    if (!sc.frame.width || !sc.frame.height || !sc.frame.stride) {
        fprintf(stderr, "output screencopy: invalid buffer geometry\n");
        goto fail;
    }

    const size_t size = (size_t)sc.frame.stride * sc.frame.height;
    char name[64];
    snprintf(name, sizeof(name), "/ext_capture_output_%d", (int)getpid());
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        goto fail;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        goto fail;
    }
    out->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out->data == MAP_FAILED) {
        out->data = NULL;
        close(fd);
        goto fail;
    }
    memset(out->data, 0, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(client->toplevel.shm, fd, (int)size);
    close(fd);
    if (!pool) {
        munmap(out->data, size);
        out->data = NULL;
        goto fail;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, (int)sc.frame.width,
        (int)sc.frame.height, (int)sc.frame.stride, sc.frame.format);
    wl_shm_pool_destroy(pool);
    if (!buffer) {
        munmap(out->data, size);
        out->data = NULL;
        goto fail;
    }
    out->width = sc.frame.width;
    out->height = sc.frame.height;
    out->stride = sc.frame.stride;
    out->format = sc.frame.format;

    zwlr_screencopy_frame_v1_copy(frame, buffer);
    wl_display_flush(client->connection.display);
    wl_buffer_destroy(buffer);

    /* Deadline-based wait (see wait_flag_on_socket): a commit with the
     * attach_render lock held must arrive; slow CI environments may need
     * hundreds of ms per composited frame. */
    const int wret = wait_flag_on_socket(client->connection.display,
                                         &sc.ready, &sc.failed, 5000);
    zwlr_screencopy_frame_v1_destroy(frame);
    if (!sc.ready) {
        fprintf(stderr, "output screencopy: no ready event within deadline (failed=%d wret=%d)\n",
                sc.failed, wret);
        free_output_frame(out);
        return 0;
    }
    return 1;
fail:
    zwlr_screencopy_frame_v1_destroy(frame);
    free_output_frame(out);
    return 0;
}

static uint32_t frame_pixel(const struct output_frame *f, uint32_t x, uint32_t y)
{
    const char *line = (const char *)f->data + (size_t)y * f->stride;
    uint32_t pixel;
    memcpy(&pixel, line + (size_t)x * 4, sizeof(pixel));
    return pixel;
}

/* Find the top-left of a solid block_size×block_size block of the given
 * color in the output frame; returns 1 when found. */
static int find_color_block(const struct output_frame *f, uint32_t shm_format,
                            int (*is_color)(const struct capture_client *, uint32_t),
                            uint32_t block_size, uint32_t *ox, uint32_t *oy)
{
    struct capture_client fmt = { 0 };
    fmt.shm_format = shm_format;
    for (uint32_t y = 0; y + block_size <= f->height; y++) {
        for (uint32_t x = 0; x + block_size <= f->width; x++) {
            if (is_color(&fmt, frame_pixel(f, x, y))
                && is_color(&fmt, frame_pixel(f, x + block_size - 1, y))
                && is_color(&fmt, frame_pixel(f, x, y + block_size - 1))
                && is_color(&fmt, frame_pixel(f, x + block_size - 1, y + block_size - 1))) {
                *ox = x;
                *oy = y;
                return 1;
            }
        }
    }
    return 0;
}

/* The compositor's main render must show the captured window exactly where
 * the compositor placed it, whether or not a capture session is running.
 * expected_x/y = green subsurface position in output coordinates. A move can
 * land mid-frame; retry a bounded number of natural frames until the visible
 * output converges to the expected position (dumping every attempt for
 * post-analysis) — a real mismatch (frozen/stale output) never converges. */
static int verify_window_on_screen(struct capture_client *client,
                                   const struct ext_capture_state *state,
                                   int dump_index)
{
    const int32_t expected_x = state->wrapper_x + SUB_POS;
    const int32_t expected_y = state->wrapper_y + state->content_y + SUB_POS;
    for (int attempt = 0; attempt < 12; attempt++) {
        struct output_frame out;
        if (!capture_output_frame(client, &out)) {
            fprintf(stderr, "output screencopy failed\n");
            return 0;
        }
        dump_output_frame(dump_index * 10 + attempt, &out);
        uint32_t gx = UINT32_MAX, gy = UINT32_MAX;
        const int found = find_color_block(&out, out.format, pixel_is_green, SUB_SIZE, &gx, &gy);
        free_output_frame(&out);
        if (found && abs((int32_t)gx - expected_x) <= 1 && abs((int32_t)gy - expected_y) <= 1)
            return 1;
        fprintf(stderr, "attempt %d: green block at (%u,%u), expected (%d,%d) — waiting for convergence\n",
                attempt, gx, gy, expected_x, expected_y);
    }
    fprintf(stderr, "main render position never converged to (%d,%d)\n", expected_x, expected_y);
    return 0;
}


/* ------------------------------------------------------------------ */
/* capture-frame pixel verification                                    */
/* ------------------------------------------------------------------ */

static uint32_t pixel_at(const struct capture_client *client, uint32_t x, uint32_t y)
{
    const uint32_t *line = (const uint32_t *)(client->target_data);
    return line[y * client->buffer_width + x];
}

/* The capture buffer must contain the whole window subtree with the window
 * content locked to the frame origin: the green subsurface always sits at
 * content offset (SUB_POS, content_y + SUB_POS) inside the frame, whatever
 * the window's position on screen is. */
static int verify_subtree_pixels(const struct capture_client *client,
                                 const struct ext_capture_state *state)
{
    /* Locate the 16x16 green subsurface block. */
    uint32_t gx = UINT32_MAX, gy = UINT32_MAX;
    for (uint32_t y = 0; y + SUB_SIZE <= client->buffer_height; y++) {
        for (uint32_t x = 0; x + SUB_SIZE <= client->buffer_width; x++) {
            if (pixel_is_green(client, pixel_at(client, x, y))) {
                gx = x;
                gy = y;
                break;
            }
        }
        if (gx != UINT32_MAX)
            break;
    }
    if (gx == UINT32_MAX) {
        fprintf(stderr, "subsurface (green block) not found in capture\n");
        return 0;
    }
    const uint32_t expected_gx = SUB_POS;
    const uint32_t expected_gy = (uint32_t)state->content_y + SUB_POS;
    if (gx != expected_gx || gy != expected_gy) {
        fprintf(stderr,
                "window misplaced in the capture: green block at (%u,%u), expected (%u,%u)\n",
                gx, gy, expected_gx, expected_gy);
        /* Dump the capture frame for post-analysis. */
        const char *dumpDir = getenv("EXT_CAPTURE_DUMP_DIR");
        if (dumpDir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/capture-misplaced.bmp", dumpDir);
            FILE *fp = fopen(path, "wb");
            if (fp) {
                const uint32_t w = client->buffer_width, h = client->buffer_height;
                const uint32_t stride = (w * 4 + 3) & ~3u;
                const uint32_t dataSize = stride * h;
                const uint32_t fileSize = 54 + dataSize;
                const uint16_t ppm = 2835;
                uint8_t hdr[54] = { 'B','M' };
                memcpy(hdr + 2, &fileSize, 4); hdr[10] = 54; hdr[14] = 40;
                memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
                hdr[26] = 1; hdr[28] = 32;
                memcpy(hdr + 38, &ppm, 2); memcpy(hdr + 42, &ppm, 2);
                fwrite(hdr, 1, 54, fp);
                for (uint32_t y = h; y-- > 0;) {
                    const uint32_t *src = (const uint32_t *)client->target_data;
                    fwrite(src + y * w, 4, w, fp);
                }
                fclose(fp);
                fprintf(stderr, "dumped %s (%ux%u)\n", path, w, h);
            }
        }
        return 0;
    }
    if (gx + SUB_SIZE > client->buffer_width
        || !pixel_is_green(client, pixel_at(client, gx + SUB_SIZE - 1, gy + SUB_SIZE - 1))) {
        fprintf(stderr, "green block at (%u,%u) is not %dx%d\n",
                gx, gy, SUB_SIZE, SUB_SIZE);
        return 0;
    }

    /* Main surface: the green block sits at (16,16) inside the red content. */
    const uint32_t cx = gx - SUB_POS;
    const uint32_t cy = gy - SUB_POS;
    if (cx + CONTENT_SIZE > client->buffer_width
        || cy + CONTENT_SIZE > client->buffer_height) {
        fprintf(stderr, "content rect (%u,%u %ux%u) outside capture buffer %ux%u\n",
                cx, cy, CONTENT_SIZE, CONTENT_SIZE,
                client->buffer_width, client->buffer_height);
        return 0;
    }

    static const uint32_t red_points[][2] = {
        { 10, 10 }, { 50, 10 }, { 10, 50 }, { 50, 50 }, { 40, 8 }, { 8, 40 },
    };
    for (size_t i = 0; i < sizeof(red_points) / sizeof(red_points[0]); i++) {
        const uint32_t px = cx + red_points[i][0];
        const uint32_t py = cy + red_points[i][1];
        if (!pixel_is_red(client, pixel_at(client, px, py))) {
            fprintf(stderr, "main surface pixel (%u,%u) is not red: 0x%08x\n",
                    px, py, pixel_at(client, px, py));
            return 0;
        }
    }

    static const uint32_t green_points[][2] = {
        { 2, 2 }, { 13, 2 }, { 2, 13 }, { 8, 8 },
    };
    for (size_t i = 0; i < sizeof(green_points) / sizeof(green_points[0]); i++) {
        const uint32_t px = cx + SUB_POS + green_points[i][0];
        const uint32_t py = cy + SUB_POS + green_points[i][1];
        if (!pixel_is_green(client, pixel_at(client, px, py))) {
            fprintf(stderr, "subsurface pixel (%u,%u) is not green: 0x%08x\n",
                    px, py, pixel_at(client, px, py));
            return 0;
        }
    }

    uint32_t red = 0, total = 0;
    for (uint32_t y = 2; y < CONTENT_SIZE - 2; y += 2) {
        for (uint32_t x = 2; x < CONTENT_SIZE - 2; x += 2) {
            if (x >= SUB_POS - 1 && x < SUB_POS + SUB_SIZE + 1
                && y >= SUB_POS - 1 && y < SUB_POS + SUB_SIZE + 1)
                continue;
            total++;
            if (pixel_is_red(client, pixel_at(client, cx + x, cy + y)))
                red++;
        }
    }
    if (red * 100 < total * 80) {
        fprintf(stderr, "main surface area coverage too low: %u/%u\n", red, total);
        return 0;
    }

    /* Decoration: the region directly above the content must be painted by
     * the compositor (opaque title bar), not the transparent shadow margin.
     * For XRGB formats there is no alpha, compare the RGB brightness. */
    const uint32_t pixel = pixel_at(client, cx + CONTENT_SIZE / 2, cy - 10);
    const unsigned char *b = (const unsigned char *)&pixel;
    const int opaque = client->shm_format == WL_SHM_FORMAT_ARGB8888
        ? b[3] > 200 : (b[0] > 200 && b[1] > 200 && b[2] > 200);
    if (!opaque) {
        fprintf(stderr, "pixel above the content is not an opaque title bar: 0x%08x\n",
                pixel);
        return 0;
    }
    return 1;
}

/* Find and bind the wl_output whose wl_output.name matches 
 * 
 * the given name (wl_output version 4). Returns NULL when not found; the
 * caller then falls back to the first advertised output. */
static void output_name_event(void *data, struct wl_output *output, const char *name)
{
    (void)output;
    strncpy((char *)data, name, 63);
    ((char *)data)[63] = '\0';
}

static void output_done_event(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_geometry_event(void *data, struct wl_output *output, int32_t x, int32_t y,
                                  int32_t pw, int32_t ph, int32_t sub, const char *make,
                                  const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)sub; (void)make; (void)model; (void)transform;
}

static void output_mode_event(void *data, struct wl_output *output, uint32_t flags,
                              int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_scale_event(void *data, struct wl_output *output, int32_t factor)
{
    (void)data; (void)output; (void)factor;
}

static void output_description_event(void *data, struct wl_output *output, const char *desc)
{
    (void)data; (void)output; (void)desc;
}

static const struct wl_output_listener output_name_listener = {
    .geometry = output_geometry_event,
    .mode = output_mode_event,
    .done = output_done_event,
    .scale = output_scale_event,
    .name = output_name_event,
    .description = output_description_event,
};

static struct wl_output *bind_output_by_name(struct client_connection *conn,
                                             const char *want)
{
    char bound[64] = { 0 };
    for (uint32_t i = 0; i < conn->global_count; ++i) {
        const struct client_global *g = &conn->globals[i];
        if (strcmp(g->interface, wl_output_interface.name) != 0 || g->version < 4)
            continue;
        struct wl_output *out = wl_registry_bind(conn->registry, g->name,
                                                 &wl_output_interface, 4);
        if (!out)
            continue;
        bound[0] = '\0';
        wl_output_add_listener(out, &output_name_listener, bound);
        wl_display_roundtrip(conn->display);
        if (strcmp(bound, want) == 0)
            return out;
        wl_output_destroy(out);
        wl_display_roundtrip(conn->display);
    }
    return NULL;
}

static void cleanup(struct capture_client *client)
{
    if (client->session)
        ext_image_copy_capture_session_v1_destroy(client->session);
    if (client->source)
        ext_image_capture_source_v1_destroy(client->source);
    if (client->screencopy_manager)
        zwlr_screencopy_manager_v1_destroy(client->screencopy_manager);
    if (client->wl_output)
        wl_output_destroy(client->wl_output);
    if (client->toplevel_list)
        ext_foreign_toplevel_list_v1_destroy(client->toplevel_list);
    if (client->capture_manager)
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(client->capture_manager);
    if (client->copy_manager)
        ext_image_copy_capture_manager_v1_destroy(client->copy_manager);
    release_target_buffer(client);
    if (client->subsurface_buffer)
        wl_buffer_destroy(client->subsurface_buffer);
    if (client->subsurface)
        wl_subsurface_destroy(client->subsurface);
    if (client->subsurface_surface)
        wl_surface_destroy(client->subsurface_surface);
    xdg_toplevel_client_destroy(&client->toplevel);
    client_disconnect(&client->connection);
}

int protocol_test_run(const char *socket_name)
{
    struct capture_client client = { 0 };
    struct ext_capture_state state = { 0 };
    int result = 1;

    if (getenv("EXT_CAPTURE_DUMP_DIR"))
        g_dump_frames = 1;

    if (!client_connect(&client.connection, socket_name))
        goto done;

    client.copy_manager = client_bind(&client.connection,
        ext_image_copy_capture_manager_v1_interface.name,
        &ext_image_copy_capture_manager_v1_interface, 1);
    client.subcompositor = client_bind(&client.connection,
        "wl_subcompositor", &wl_subcompositor_interface, 1);
    client.decoration_manager = client_bind(&client.connection,
        "zxdg_decoration_manager_v1", &zxdg_decoration_manager_v1_interface, 1);

    if (wl_display_roundtrip(client.connection.display) < 0
        || wl_display_roundtrip(client.connection.display) < 0)
        goto done;

    if (!client.copy_manager || !client.subcompositor || !client.decoration_manager) {
        fprintf(stderr, "capture globals missing: copy=%p sub=%p deco=%p\n",
                (void *)client.copy_manager, (void *)client.subcompositor,
                (void *)client.decoration_manager);
        goto done;
    }

    /* Mapped production window with SSD decoration and a subsurface. */
    if (!create_mapped_window(&client)) {
        fprintf(stderr, "failed to create mapped window\n");
        goto done;
    }
    if (!add_subsurface(&client)) {
        fprintf(stderr, "failed to add subsurface\n");
        goto done;
    }

    /* Bind the toplevel list AFTER the window is mapped: its bind handler
     * enumerates existing toplevels, so the handle arrives without races. */
    /* Early state query: the owning output's name is needed to bind the
     * correct wl_output before any screencopy. */
    if (!invoke_on_server_thread(ext_capture_query_state, &state) || !state.wrapper_ready) {
        fprintf(stderr, "wrapper never became ready\n");
        goto done;
    }
    /* The owning output may still be disabled during the compositor's initial
     * scan/restore (async) — wlr-screencopy fails on disabled outputs. Wait
     * for the enable to land before recording. */
    for (int i = 0; !state.output_enabled && i < 100; i++) {
        wl_display_roundtrip(client.connection.display);
        if (!invoke_on_server_thread(ext_capture_query_state, &state))
            break;
    }
    if (!state.output_enabled) {
        struct ext_capture_wait_state est = { 0 };
        invoke_on_server_thread(ext_capture_enable_output, &est);
        if (!invoke_on_server_thread(ext_capture_query_state, &state))
            fprintf(stderr, "re-query after enable failed\n");
    }
    if (!state.output_enabled)
        fprintf(stderr, "warning: owning output still disabled, screencopy will fail\n");
    client.toplevel_list = client_bind(&client.connection,
        ext_foreign_toplevel_list_v1_interface.name,
        &ext_foreign_toplevel_list_v1_interface, 1);
    client.capture_manager = client_bind(&client.connection,
        ext_foreign_toplevel_image_capture_source_manager_v1_interface.name,
        &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    /* The listener must be attached before any further roundtrip: the server
     * answers the bind with the enumeration of existing toplevels, and those
     * events would otherwise be dispatched (and dropped) unlistened. */
    ext_foreign_toplevel_list_v1_add_listener(client.toplevel_list,
        &toplevel_list_listener, &client);
    client.wl_output = state.output_name[0]
        ? bind_output_by_name(&client.connection, state.output_name) : NULL;
    if (!client.wl_output)
        client.wl_output = client_bind(&client.connection, "wl_output",
                                       &wl_output_interface, 1);
    client.screencopy_manager = client_bind(&client.connection,
        zwlr_screencopy_manager_v1_interface.name,
        &zwlr_screencopy_manager_v1_interface, 1);
    if (!client.toplevel_list || !client.capture_manager) {
        fprintf(stderr, "capture globals missing: list=%p capture=%p\n",
                (void *)client.toplevel_list, (void *)client.capture_manager);
        goto done;
    }
    for (int i = 0; i < 50 && !client.handle_seen; i++) {
        if (wl_display_roundtrip(client.connection.display) < 0)
            break;
    }
    if (!client.handle_seen) {
        fprintf(stderr, "no foreign toplevel handle seen\n");
        goto done;
    }

    if (!invoke_on_server_thread(ext_capture_query_state, &state) || !state.wrapper_ready) {
        fprintf(stderr, "wrapper never became ready\n");
        goto done;
    }
    if (!state.output_ready || !state.wrapper_visible || !state.surface_item_visible
        || !state.content_visible || !state.content_in_paint_order) {
        fprintf(stderr, "window not rendered normally: out=%d wrapper=%d item=%d content=%d paint=%d\n",
                state.output_ready, state.wrapper_visible, state.surface_item_visible,
                state.content_visible, state.content_in_paint_order);
        goto done;
    }
    if (state.wrapper_height <= CONTENT_SIZE) {
        fprintf(stderr, "no decoration area appeared (wrapper %dx%d)\n",
                state.wrapper_width, state.wrapper_height);
        goto done;
    }

    client.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        client.capture_manager, client.toplevel_handle);
    if (!client.source) {
        fprintf(stderr, "create_source failed\n");
        goto done;
    }

    client.session = ext_image_copy_capture_manager_v1_create_session(
        client.copy_manager, client.source, 0);
    if (!client.session)
        goto done;
    ext_image_copy_capture_session_v1_add_listener(client.session, &session_listener, &client);

    if (!wait_session_done(&client)) {
        fprintf(stderr, "session constraints missing\n");
        goto done;
    }
    fprintf(stderr, "session constraints: %ux%u fmt 0x%x\n",
            client.buffer_width, client.buffer_height, client.shm_format);

    /* The advertised size must be the whole window subtree (title bar +
     * main surface), without the shadow margins. */
    if (client.buffer_width != (uint32_t)state.wrapper_width
        || client.buffer_height != (uint32_t)state.wrapper_height) {
        fprintf(stderr, "capture size %ux%u != window subtree size %dx%d\n",
                client.buffer_width, client.buffer_height,
                state.wrapper_width, state.wrapper_height);
        goto done;
    }
    if (client.buffer_height > (uint32_t)(state.content_y + CONTENT_SIZE)) {
        fprintf(stderr, "capture carries margins (height %u, content needs %d)\n",
                client.buffer_height, state.content_y + CONTENT_SIZE);
        goto done;
    }

    if (!create_target_buffer(&client))
        goto done;

    /* Baseline A/B: the same move-and-verify WITHOUT any capture session.
     * This isolates whether the output's move-following behavior changes
     * when a capture session is active. */
    for (int i = 0; i < 2; i++) {
        struct ext_capture_move_request req = { 500 + i * 20, 80 + i * 10 };
        if (!invoke_on_server_thread(ext_capture_move_window, &req)) {
            fprintf(stderr, "baseline move failed\n");
            goto done;
        }
        wl_display_flush(client.connection.display);
        struct ext_capture_wait_state wait;
        invoke_on_server_thread(ext_capture_wait_render, &wait);
        if (!invoke_on_server_thread(ext_capture_query_state, &state))
            goto done;
        if (!verify_window_on_screen(&client, &state, 50 + i))
            goto done;
    }
    fprintf(stderr, "baseline (no capture): output follows moves\n");

    /* Baseline: how many natural frames happen without capture activity. */
    {
        struct ext_capture_render_delta delta;
        if (!invoke_on_server_thread(ext_capture_render_end_delta, &delta)) {
            fprintf(stderr, "render-end probe failed\n");
            goto done;
        }
    }

    /* A short recording while moving the window: the frame content must be
     * locked to the frame origin and the main render must stay put. */
    for (int i = 0; i < 3; i++) {
        if (!capture_one_frame(&client))
            goto done;
        if (i == 0 && !verify_subtree_pixels(&client, &state))
            goto done;
    }

    /* Move the window while capturing: the capture frame must NOT move and
     * the main render must place the window exactly where the compositor
     * put it (the fix for the "captured window moves / render jumps" bug). */
    {
        static const int moves[][2] = { { 40, 30 }, { 200, 100 }, { 300, 60 }, { 90, 200 } };
        for (size_t m = 0; m < sizeof(moves) / sizeof(moves[0]); m++) {
            struct ext_capture_move_request req = { moves[m][0], moves[m][1] };
            if (!invoke_on_server_thread(ext_capture_move_window, &req)) {
                fprintf(stderr, "move_window invoke failed\n");
                goto done;
            }
            wl_display_flush(client.connection.display);
            /* Wait for the natural frames to apply the move (no forced render). */
            struct ext_capture_wait_state wait;
            invoke_on_server_thread(ext_capture_wait_render, &wait);

            if (!invoke_on_server_thread(ext_capture_query_state, &state)) {
                fprintf(stderr, "query after move failed\n");
                goto done;
            }
            if ((int32_t)state.wrapper_x != moves[m][0] || (int32_t)state.wrapper_y != moves[m][1]) {
                fprintf(stderr, "wrapper did not move to (%d,%d): at (%d,%d)\n",
                        moves[m][0], moves[m][1], state.wrapper_x, state.wrapper_y);
                goto done;
            }
            /* Capture frame: content must stay locked to the frame origin. */
            if (!capture_one_frame(&client))
                goto done;
            if (!verify_subtree_pixels(&client, &state))
                goto done;
            /* Main render: window exactly where placed, no jumping. */
            if (!verify_window_on_screen(&client, &state, (int)m))
                goto done;
        }
        fprintf(stderr, "move-invariance: %d capture frames, content locked, main render stable\n",
                client.frames_copied);
    }

    /* Resize the captured window while the session is running: the capture
     * constraints must follow, and the main render must not jump. */
    {
        struct ext_capture_render_delta delta;
        invoke_on_server_thread(ext_capture_render_end_delta, &delta);
        const int baseline_total = delta.total;

        struct wl_buffer *bigger = make_solid_buffer(client.toplevel.shm,
                                                     80, 80, MAIN_COLOR, NULL);
        if (!bigger)
            goto done;
        wl_surface_attach(client.toplevel.surface, bigger, 0, 0);
        wl_surface_damage(client.toplevel.surface, 0, 0, 80, 80);
        wl_surface_commit(client.toplevel.surface);
        wl_display_flush(client.connection.display);

        struct ext_capture_state resized = { 0 };
        int resized_ok = 0;
        for (int i = 0; i < 100 && !resized_ok; i++) {
            if (wl_display_roundtrip(client.connection.display) < 0)
                break;
            if (!invoke_on_server_thread(ext_capture_query_state, &resized))
                break;
            resized_ok = resized.wrapper_ready && resized.wrapper_width == 80
                && (uint32_t)resized.wrapper_height
                    == (uint32_t)(resized.content_y + 80);
        }
        wl_buffer_destroy(bigger);
        if (!resized_ok) {
            fprintf(stderr, "window resize not picked up (wrapper %dx%d)\n",
                    resized.wrapper_width, resized.wrapper_height);
            goto done;
        }
        fprintf(stderr, "resized to %dx%d (title bar %d)\n",
                resized.wrapper_width, resized.wrapper_height, resized.content_y);

        /* The capture source must announce the new size (constraints_update). */
        int got_new_size = 0;
        for (int i = 0; i < 100 && !got_new_size; i++) {
            if (wl_display_roundtrip(client.connection.display) < 0)
                break;
            got_new_size = client.buffer_width == (uint32_t)resized.wrapper_width
                && client.buffer_height == (uint32_t)resized.wrapper_height;
        }
        if (!got_new_size) {
            fprintf(stderr, "capture size not updated after resize: %ux%u vs %dx%d\n",
                    client.buffer_width, client.buffer_height,
                    resized.wrapper_width, resized.wrapper_height);
            goto done;
        }
        release_target_buffer(&client);
        if (!create_target_buffer(&client))
            goto done;
        if (!capture_one_frame(&client)
            || !verify_subtree_pixels(&client, &resized))
            goto done;

        /* The on-screen position must be unchanged by capture + resize. */
        if (!verify_window_on_screen(&client, &resized, 90))
            goto done;

        /* Zero-coupling check: the whole move+resize sequence must not have
         * turned the capture into a render driver. The moves and the client's
         * own resize commit legitimately schedule a handful of frames (each
         * move/resize ≈1); a capture-induced render loop would show hundreds. */
        invoke_on_server_thread(ext_capture_render_end_delta, &delta);
        fprintf(stderr, "render ends during move+resize phase: total=%d (baseline %d), delta=%d\n",
                delta.total, baseline_total, delta.delta);
        if (delta.delta > 20) {
            fprintf(stderr, "capture perturbed the output frame loop: %d extra renders\n",
                    delta.delta);
            goto done;
        }
    }

    /* Capture end: the source must be reusable by a new session (stop/start). */
    ext_image_copy_capture_session_v1_destroy(client.session);
    client.session = NULL;
    client.have_size = 0;
    client.have_shm_format = 0;
    client.session_done = 0;
    client.session = ext_image_copy_capture_manager_v1_create_session(
        client.copy_manager, client.source, 0);
    if (!client.session)
        goto done;
    ext_image_copy_capture_session_v1_add_listener(client.session, &session_listener, &client);
    if (!wait_session_done(&client) || !capture_one_frame(&client)) {
        fprintf(stderr, "restarted session failed\n");
        goto done;
    }

    /* The captured window must still be alive and rendered normally. */
    memset(&state, 0, sizeof(state));
    if (!invoke_on_server_thread(ext_capture_query_state, &state) || !state.wrapper_ready
        || !state.wrapper_visible || !state.surface_item_visible || !state.content_visible) {
        fprintf(stderr, "window no longer rendered normally after capture\n");
        goto done;
    }

    fprintf(stderr, "all capture frames copied: %d\n", client.frames_copied);
    result = 0;

done:
    if (result != 0) {
        fprintf(stderr,
                "ext toplevel capture failed: size=%ux%u fmt=0x%x state=(wrapper=%d out=%d vis=%d item=%d content=%d paint=%d %dx%d +%d+%d)\n",
                client.buffer_width, client.buffer_height, client.shm_format,
                state.wrapper_ready, state.output_ready, state.wrapper_visible,
                state.surface_item_visible, state.content_visible,
                state.content_in_paint_order, state.wrapper_width,
                state.wrapper_height, state.content_x, state.content_y);
    }
    cleanup(&client);
    return result;
}