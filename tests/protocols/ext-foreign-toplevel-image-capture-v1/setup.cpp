// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#include "ext-foreign-toplevel-image-capture-v1.h"
#include "server-bridge.h"
#include "seat/helper.h"
#include "surface/surfacewrapper.h"
#include "core/rootsurfacecontainer.h"
#include "core/shellhandler.h"

#include <wbackend.h>
#include <woutputrenderwindow.h>
#include <wsurfaceitem.h>

namespace {
SurfaceWrapper *g_wrapper = nullptr;
}

void protocol_test_setup(Helper *helper)
{
    add_headless_output(helper->backend(), false);
    QObject::connect(helper->shellHandler(),
                     &ShellHandler::surfaceWrapperAdded,
                     helper,
                     [helper](SurfaceWrapper *wrapper) {
                         if (wrapper->type() != SurfaceWrapper::Type::XdgToplevel)
                             return;
                         g_wrapper = wrapper;
                     });
}

void ext_capture_query_state(void *data)
{
    auto *state = static_cast<ext_capture_state *>(data);
    auto *helper = Helper::instance();
    state->output_ready = helper->rootSurfaceContainer()->outputs().isEmpty() ? 0 : 1;
    state->wrapper_ready = g_wrapper ? 1 : 0;
    if (!g_wrapper)
        return;

    state->wrapper_visible = g_wrapper->isVisible() ? 1 : 0;
    state->surface_item_visible = g_wrapper->surfaceItem()
        && g_wrapper->surfaceItem()->isVisible() ? 1 : 0;

    auto *content = g_wrapper->surfaceItem()
        ? g_wrapper->surfaceItem()->findItemContent() : nullptr;
    state->content_visible = content && content->isVisible() ? 1 : 0;
    const auto paintOrder = WOutputRenderWindow::paintOrderItemList(
        helper->window()->contentItem(), [](QQuickItem *) { return true; });
    state->content_in_paint_order = content && paintOrder.contains(content) ? 1 : 0;

    // The capture source renders exactly this subtree geometry.
    const QRectF bounds = g_wrapper->boundingRect();
    state->wrapper_width = qRound(bounds.width());
    state->wrapper_height = qRound(bounds.height());
    if (g_wrapper->surfaceItem()) {
        // Scale is 1 on the headless output; report in buffer coordinates.
        const QPointF offset = g_wrapper->surfaceItem()->position() - bounds.topLeft();
        state->content_x = qRound(offset.x());
        state->content_y = qRound(offset.y());
    }
}

void ext_capture_force_render(void *data)
{
    Q_UNUSED(data);
    Helper::instance()->window()->render();
}
