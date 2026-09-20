// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ext_capture_state {
    int wrapper_ready;
    int output_ready;
    int wrapper_visible;
    int surface_item_visible;
    int content_visible;
    int content_in_paint_order;
    int wrapper_width;
    int wrapper_height;
    int content_x; /* main-surface top-left inside the capture buffer */
    int content_y;
};

// Filled on the server thread; called by the client via the test bridge.
void ext_capture_query_state(void *data);

// Forces one production render pass (like capture-desktop does) so capture
// frames are produced deterministically in the headless environment.
void ext_capture_force_render(void *data);

#ifdef __cplusplus
}
#endif
