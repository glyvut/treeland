// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#include "client-connection.h"
#include "server-bridge-api.h"
#include "treeland-app-id-resolver-unstable-v2-client-protocol.h"
#include "treeland-prelaunch-splash-desktop-v2.h"
#include "treeland-prelaunch-splash-unstable-v2-client-protocol.h"
#include "xdg-toplevel-client.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern void prelaunch_splash_desktop_read_state(void *data);
extern void prelaunch_splash_desktop_wait_for_creation(void *data);
extern void prelaunch_splash_desktop_wait_for_destruction(void *data);
extern void prelaunch_splash_desktop_wait_converted(void *data);

static const char *const splash_app_id = "org.deepin.treeland.protocol.splash";
static const char *const icon_app_id = "org.deepin.treeland.protocol.splash-icon";

#define ICON_WIDTH 16
#define ICON_HEIGHT 16
#define ICON_ARGB 0xffff0000u /* opaque red, ARGB8888 */
/* The splash wrapper is 800x600; the real window must match to convert without
   a geometry animation. */
#define SPLASH_WINDOW_WIDTH 800
#define SPLASH_WINDOW_HEIGHT 600

struct resolver_state {
    uint32_t request_id;
    int identify_count;
    int pidfd;
};

static void identify_request(void *data,
                             struct treeland_app_id_resolver_v2 *resolver,
                             uint32_t request_id,
                             int32_t pidfd)
{
    (void)resolver;
    struct resolver_state *state = data;
    state->request_id = request_id;
    ++state->identify_count;
    state->pidfd = pidfd;
}

static const struct treeland_app_id_resolver_v2_listener resolver_listener = {
    .identify_request = identify_request,
};

static struct wl_buffer *create_solid_buffer(struct wl_shm *shm,
                                             int width,
                                             int height,
                                             uint32_t argb)
{
    const int stride = width * 4;
    const size_t size = (size_t)stride * height;
    char name[64];
    snprintf(name, sizeof(name), "/treeland_splash_desktop_%d_%dx%d", (int)getpid(), width, height);

    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return NULL;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    uint32_t *pixels = data;
    for (size_t i = 0; i < (size_t)width * height; ++i)
        pixels[i] = argb;

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(data, size);
    close(fd);
    return buffer;
}

static int state_has_production_splash(const struct prelaunch_splash_desktop_state *state)
{
    return state->output_ready && state->wrapper_created && state->wrapper_in_workspace
        && state->wrapper_is_splash && state->wrapper_has_qml_item && state->wrapper_width == 800
        && state->wrapper_height == 600 && strcmp(state->app_id, splash_app_id) == 0;
}

static int state_has_surviving_icon(const struct prelaunch_splash_desktop_state *state)
{
    return state->icon_ready && state->icon_width == ICON_WIDTH
        && state->icon_height == ICON_HEIGHT;
}

#define MAP_STEP(step) \
    do { \
        if (!(step)) { \
            fprintf(stderr, "map_window failed: %s\n", #step); \
            wl_buffer_destroy(buffer); \
            return 0; \
        } \
    } while (0)

static int map_window(struct client_connection *connection,
                      struct resolver_state *resolver_state,
                      struct xdg_toplevel_client *toplevel,
                      struct treeland_app_id_resolver_v2 *resolver,
                      const char *app_id,
                      int width,
                      int height)
{
    struct wl_buffer *buffer = NULL;
    const int requests_before = resolver_state->identify_count;

    MAP_STEP(xdg_toplevel_client_create_pending(connection, toplevel));
    MAP_STEP(wl_display_roundtrip(connection->display) >= 0);

    if (resolver_state->identify_count > requests_before) {
        treeland_app_id_resolver_v2_respond(resolver,
                                            resolver_state->request_id,
                                            app_id,
                                            "org.deepin.Sandbox");
        if (resolver_state->pidfd >= 0) {
            close(resolver_state->pidfd);
            resolver_state->pidfd = -1;
        }
    }

    /* The size has to be attached before the configure: a same-size window
       converts straight through, a differing one would need the geometry
       animation, which the headless software renderer never schedules. */
    for (int roundtrip = 0; roundtrip < 50 && !toplevel->configured; ++roundtrip)
        MAP_STEP(wl_display_roundtrip(connection->display) >= 0);
    MAP_STEP(toplevel->configured);
    MAP_STEP(xdg_toplevel_client_ack_latest_configure(connection, toplevel));
    buffer = create_solid_buffer(toplevel->shm, width, height, 0xffffffffu);
    MAP_STEP(buffer != NULL);
    wl_surface_attach(toplevel->surface, buffer, 0, 0);
    wl_surface_damage(toplevel->surface, 0, 0, width, height);
    wl_surface_commit(toplevel->surface);
    MAP_STEP(wl_display_roundtrip(connection->display) >= 0);
    wl_buffer_destroy(buffer);
    return 1;
}

int protocol_test_run(const char *socket_name)
{
    struct client_connection connection;
    struct xdg_toplevel_client splash_window = { 0 };
    struct resolver_state resolver_state = { .pidfd = -1 };
    struct prelaunch_splash_desktop_state desktop_state = { 0 };
    struct treeland_app_id_resolver_manager_v2 *resolver_manager = NULL;
    struct treeland_app_id_resolver_v2 *resolver = NULL;
    struct treeland_prelaunch_splash_manager_v2 *splash_manager = NULL;
    struct treeland_prelaunch_splash_v2 *splash = NULL;
    struct wl_shm *shm = NULL;
    struct wl_buffer *icon = NULL;
    int observed = 0;
    int result = 1;
    const char *stage = "connect";

    if (!client_connect(&connection, socket_name))
        goto done;

    stage = "bind globals";
    resolver_manager = client_bind(&connection,
                                   "treeland_app_id_resolver_manager_v2",
                                   &treeland_app_id_resolver_manager_v2_interface,
                                   1);
    splash_manager = client_bind(&connection,
                                 "treeland_prelaunch_splash_manager_v2",
                                 &treeland_prelaunch_splash_manager_v2_interface,
                                 2);
    shm = client_bind(&connection, "wl_shm", &wl_shm_interface, 1);
    if (!resolver_manager || !splash_manager || !shm)
        goto done;

    resolver = treeland_app_id_resolver_manager_v2_get_resolver(resolver_manager);
    if (!resolver
        || treeland_app_id_resolver_v2_add_listener(resolver, &resolver_listener, &resolver_state))
        goto done;

    /* Phase 1: the splash wrapper and its teardown. */
    stage = "create splash";
    icon = create_solid_buffer(shm, ICON_WIDTH, ICON_HEIGHT, ICON_ARGB);
    if (!icon)
        goto done;
    splash = treeland_prelaunch_splash_manager_v2_create_splash(splash_manager,
                                                              splash_app_id,
                                                              "protocol-instance",
                                                              "org.deepin.Sandbox",
                                                              icon);
    if (!splash || wl_display_roundtrip(connection.display) < 0)
        goto done;

    /* The compositor owns the icon from here; the client drops its reference like AM does. */
    wl_buffer_destroy(icon);
    icon = NULL;

    if (!invoke_on_server_thread(prelaunch_splash_desktop_wait_for_creation, &observed) || !observed
        || !invoke_on_server_thread(prelaunch_splash_desktop_read_state, &desktop_state)
        || !state_has_production_splash(&desktop_state)
        || !state_has_surviving_icon(&desktop_state)) {
        stage = "splash wrapper";
        goto done;
    }
    observed = 0;

    treeland_prelaunch_splash_v2_destroy(splash);
    splash = NULL;
    if (wl_display_roundtrip(connection.display) < 0
        || !invoke_on_server_thread(prelaunch_splash_desktop_wait_for_destruction, &observed)
        || !observed
        || !invoke_on_server_thread(prelaunch_splash_desktop_read_state, &desktop_state)
        || !desktop_state.wrapper_destroyed || desktop_state.wrapper_in_workspace) {
        stage = "splash teardown";
        goto done;
    }
    observed = 0;
    desktop_state = (struct prelaunch_splash_desktop_state){ 0 };

    /* Phase 2: the icon outliving the splash and the conversion into the real window. */
    stage = "create icon splash";
    icon = create_solid_buffer(shm, ICON_WIDTH, ICON_HEIGHT, ICON_ARGB);
    if (!icon)
        goto done;
    splash = treeland_prelaunch_splash_manager_v2_create_splash(splash_manager,
                                                               icon_app_id,
                                                               "icon-instance",
                                                               "org.deepin.Sandbox",
                                                               icon);
    if (!splash || wl_display_roundtrip(connection.display) < 0)
        goto done;
    wl_buffer_destroy(icon);
    icon = NULL;

    if (!invoke_on_server_thread(prelaunch_splash_desktop_wait_for_creation, &observed) || !observed) {
        stage = "icon splash wrapper";
        goto done;
    }
    observed = 0;

    stage = "convert splash into the app window";
    if (!map_window(&connection,
                    &resolver_state,
                    &splash_window,
                    resolver,
                    icon_app_id,
                    SPLASH_WINDOW_WIDTH,
                    SPLASH_WINDOW_HEIGHT))
        goto done;
    if (!invoke_on_server_thread(prelaunch_splash_desktop_wait_converted, &observed) || !observed
        || !invoke_on_server_thread(prelaunch_splash_desktop_read_state, &desktop_state)
        || !desktop_state.converted_to_xdg || !desktop_state.splash_item_gone
        || !state_has_surviving_icon(&desktop_state)) {
        stage = "splash conversion";
        goto done;
    }

    result = 0;
done:
    if (result != 0) {
        fprintf(stderr,
                "prelaunch splash desktop failed at %s: splash=(created=%d xdg=%d item-gone=%d "
                "icon=%d %dx%d) destruction=(destroyed=%d in-workspace=%d) output=%d\n",
                stage,
                desktop_state.wrapper_created,
                desktop_state.converted_to_xdg,
                desktop_state.splash_item_gone,
                desktop_state.icon_ready,
                desktop_state.icon_width,
                desktop_state.icon_height,
                desktop_state.wrapper_destroyed,
                desktop_state.wrapper_in_workspace,
                desktop_state.output_ready);
    }
    if (icon)
        wl_buffer_destroy(icon);
    if (splash_window.surface)
        xdg_toplevel_client_destroy(&splash_window);
    if (splash)
        treeland_prelaunch_splash_v2_destroy(splash);
    if (resolver)
        treeland_app_id_resolver_v2_destroy(resolver);
    if (splash_manager)
        treeland_prelaunch_splash_manager_v2_destroy(splash_manager);
    if (resolver_manager)
        treeland_app_id_resolver_manager_v2_destroy(resolver_manager);
    if (resolver_state.pidfd >= 0)
        close(resolver_state.pidfd);
    client_disconnect(&connection);
    return result;
}
