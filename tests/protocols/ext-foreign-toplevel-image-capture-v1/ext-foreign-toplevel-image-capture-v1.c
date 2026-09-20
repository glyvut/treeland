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
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern void ext_capture_query_state(void *data);
extern void ext_capture_force_render(void *data);

/* The captured window: a 64x64 opaque-red main surface with a 16x16
 * opaque-green subsurface stacked at (16,16) inside the content. The capture
 * must contain the whole subtree (decoration + main surface + subsurface). */
#define CONTENT_SIZE 64
#define SUB_SIZE 16
#define SUB_POS 16
#define MAIN_COLOR 0xffff0000u /* ARGB: opaque red */
#define SUB_COLOR 0xff00ff00u /* ARGB: opaque green */

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
    ((struct capture_client *)data)->frame_ready = 1;
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
    snprintf(name, sizeof(name), "/ext_capture_target_%d", (int)getpid());
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

static int wait_session_done(struct capture_client *client)
{
    for (int i = 0; i < 100 && !client->session_done; i++) {
        if (wl_display_dispatch(client->connection.display) < 0)
            return 0;
    }
    return client->session_done && client->have_size && client->have_shm_format;
}

/* Returns 1 when one capture frame completes with a ready event. */
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

    /* Deterministically drive one production render pass on the server. */
    if (!invoke_on_server_thread(ext_capture_force_render, NULL))
        return 0;

    while (!client->frame_ready && !client->frame_failed) {
        if (wl_display_dispatch(client->connection.display) < 0)
            return 0;
    }
    ext_image_copy_capture_frame_v1_destroy(frame);
    return client->frame_ready;
}

/* ------------------------------------------------------------------ */
/* pixel verification                                                  */
/* ------------------------------------------------------------------ */

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

static uint32_t pixel_at(const struct capture_client *client, uint32_t x, uint32_t y)
{
    const uint32_t *line = (const uint32_t *)(client->target_data);
    return line[y * client->buffer_width + x];
}

/* The capture buffer must contain the whole window subtree: the client's
 * main surface (red), its subsurface (green on top of the red content) and
 * the compositor-side decoration (title bar) around it. The content region
 * is located through the subsurface, which cannot be confused with the
 * decoration chrome. */
static int verify_subtree_pixels(const struct capture_client *client)
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

static void cleanup(struct capture_client *client)
{
    if (client->session)
        ext_image_copy_capture_session_v1_destroy(client->session);
    if (client->source)
        ext_image_capture_source_v1_destroy(client->source);
    if (client->toplevel_list)
        ext_foreign_toplevel_list_v1_destroy(client->toplevel_list);
    if (client->capture_manager)
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(client->capture_manager);
    if (client->copy_manager)
        ext_image_copy_capture_manager_v1_destroy(client->copy_manager);
    if (client->target_buffer)
        wl_buffer_destroy(client->target_buffer);
    if (client->target_data)
        munmap(client->target_data, client->target_size);
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
    client.toplevel_list = client_bind(&client.connection,
        ext_foreign_toplevel_list_v1_interface.name,
        &ext_foreign_toplevel_list_v1_interface, 1);
    client.capture_manager = client_bind(&client.connection,
        ext_foreign_toplevel_image_capture_source_manager_v1_interface.name,
        &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    if (!client.toplevel_list || !client.capture_manager) {
        fprintf(stderr, "capture globals missing: list=%p capture=%p\n",
                (void *)client.toplevel_list, (void *)client.capture_manager);
        goto done;
    }
    ext_foreign_toplevel_list_v1_add_listener(client.toplevel_list,
        &toplevel_list_listener, &client);
    if (wl_display_roundtrip(client.connection.display) < 0 || !client.handle_seen) {
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

    /* The advertised size must be the whole window subtree (decoration +
     * main surface), not just the 64x64 client surface. */
    if (client.buffer_width != (uint32_t)state.wrapper_width
        || client.buffer_height != (uint32_t)state.wrapper_height) {
        fprintf(stderr, "capture size %ux%u != window subtree size %dx%d\n",
                client.buffer_width, client.buffer_height,
                state.wrapper_width, state.wrapper_height);
        goto done;
    }

    if (!create_target_buffer(&client))
        goto done;

    /* A short recording: several frames in a row. */
    for (int i = 0; i < 3; i++) {
        if (!capture_one_frame(&client)) {
            fprintf(stderr, "frame %d failed (ready=%d failed=%d reason=%u)\n",
                    i, client.frame_ready, client.frame_failed, client.frame_fail_reason);
            goto done;
        }
        if (i == 0) {
            if (!verify_subtree_pixels(&client))
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
