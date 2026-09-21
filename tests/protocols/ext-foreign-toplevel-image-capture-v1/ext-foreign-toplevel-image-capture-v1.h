// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ext_capture_state {
    int output_ready;
    int wrapper_ready;
    int wrapper_visible;
    int surface_item_visible;
    int content_visible;
    int content_in_paint_order;
    int wrapper_width;
    int wrapper_height;
    int wrapper_x;
    int wrapper_y;
    int content_x;
    int content_y;
    int titlebar_height;
};

struct ext_capture_move_request {
    int x;
    int y;
};

struct ext_capture_render_delta {
    int delta;
    int total;
};

struct ext_capture_wait_state {
    int ok;
};

#ifdef __cplusplus
}
#endif