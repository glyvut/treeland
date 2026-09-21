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
#include <QPointer>
#include <QTimer>

namespace {
SurfaceWrapper *g_wrapper = nullptr;
// Aggregated output render statistics: the zero-coupling invariant is
// "capturing never adds work to the physical output frame loop".
struct OutputStats {
    int renderEndCount = 0;
    QList<QPointer<WOutput>> committed;
};
OutputStats g_stats;
QElapsedTimer g_clock;
quint64 g_renderEndsAtLastCheck = 0;
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
                         wrapper->disableWindowAnimation();
                         g_wrapper = wrapper;
                     });
    // Count natural compositor frames. renderEnd fires once per render pass;
    // the zero-coupling test asserts capture activity does not increase it.
    QObject::connect(helper->window(), &WOutputRenderWindow::renderEnd, helper,
                     [](const QList<QPointer<WOutput>> &committed) {
                         g_stats.renderEndCount++;
                         g_stats.committed.append(committed);
                     });
    g_clock.start();
}

// Move the captured window to (x, y) on its output (deterministic, no
// animations: disableWindowAnimation is set in setup).
extern "C" void ext_capture_move_window(void *data)
{
    struct ext_capture_move_request *req = static_cast<struct ext_capture_move_request *>(data);
    if (!g_wrapper || !req)
        return;
    g_wrapper->setPosition(QPointF(req->x, req->y));
}

extern "C" void ext_capture_query_state(void *data)
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

    auto *surfaceItem = g_wrapper->surfaceItem();
    const QRectF bounds = surfaceItem ? surfaceItem->boundingRect()
                                      : g_wrapper->boundingRect();
    state->wrapper_width = qRound(bounds.width());
    state->wrapper_height = qRound(bounds.height());
    const QPointF scenePos = g_wrapper->mapToScene(QPointF(0, 0));
    state->wrapper_x = qRound(scenePos.x());
    state->wrapper_y = qRound(scenePos.y());
    if (surfaceItem) {
        const QPointF itemOrigin = surfaceItem->mapFromItem(content, QPointF(0, 0));
        const QPointF offset = itemOrigin - bounds.topLeft();
        state->content_x = qRound(offset.x());
        state->content_y = qRound(offset.y());
        state->titlebar_height = state->content_y;
    }
    fprintf(stderr, "[query] wrapper=(%d,%d) bounds=%dx%d renderEnds=%d\n",
            state->wrapper_x, state->wrapper_y,
            state->wrapper_width, state->wrapper_height,
            g_stats.renderEndCount);
}

// Zero-coupling probe: how many natural compositor frames happened since the
// last call. A capture session (start, request_frame, resize while capturing,
// copy bursts) must never increase this on its own.
extern "C" void ext_capture_render_end_delta(void *data)
{
    auto *state = static_cast<ext_capture_render_delta *>(data);
    // Flush queued events first: any capture-induced update would schedule a
    // frame and be accounted before this probe returns.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const quint64 now = static_cast<quint64>(g_stats.renderEndCount);
    state->delta = static_cast<int>(now - g_renderEndsAtLastCheck);
    state->total = static_cast<int>(now);
    g_renderEndsAtLastCheck = now;
}

// Wait for at least \a frames natural compositor frames (bounded). Used to
// observe, never to force, the frame loop.
extern "C" void ext_capture_wait_render(void *data)
{
    auto *state = static_cast<ext_capture_wait_state *>(data);
    auto *helper = Helper::instance();
    if (!helper || !helper->window()) {
        state->ok = 0;
        return;
    }
    const quint64 start = static_cast<quint64>(g_stats.renderEndCount);
    QElapsedTimer timer;
    timer.start();
    while (static_cast<quint64>(g_stats.renderEndCount) < start + 2 && timer.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    state->ok = static_cast<quint64>(g_stats.renderEndCount) >= start + 2 ? 1 : 0;
    fprintf(stderr, "[wait] start=%llu end=%llu ok=%d (%lld ms)\n",
            (unsigned long long)start,
            (unsigned long long)g_stats.renderEndCount,
            state->ok, timer.elapsed());
}