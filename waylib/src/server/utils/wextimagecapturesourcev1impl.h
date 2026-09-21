// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wlr_fwd.h>
#include <wglobal.h>
#include <wlr_all.h>

#include <QObject>
#include <QRectF>
#include <QSize>

WAYLIB_SERVER_BEGIN_NAMESPACE

class WSurfaceItem;
class WOutput;
class WOutputRenderWindow;
class CaptureBufferRenderer;

// Captures the whole window subtree owned by \a surfaceItem (client surface,
// its subsurfaces and the server-side title bar/border attached to it), while
// excluding the wrapper-level shadow.  The snapshot is produced by a
// standalone WBufferRenderer that is never attached to an output helper and
// only runs inside natural compositor frames, so capturing never schedules,
// locks or commits a physical output: the main render is completely
// unaffected by a running capture session (no extra needs_frame, no extra
// passes, no state-only commits).
class WAYLIB_SERVER_EXPORT WExtImageCaptureSourceV1Impl : public QObject
{
    Q_OBJECT
public:
    explicit WExtImageCaptureSourceV1Impl(WSurfaceItem *surfaceItem,
                                          WOutput *output,
                                          wl_client *client);
    ~WExtImageCaptureSourceV1Impl();

    wlr_ext_image_capture_source_v1 *handle() { return &source; }

private:
    static const struct wlr_ext_image_capture_source_v1_interface impl;
    void start(bool with_cursors);
    void stop();
    void schedule_frame(bool schedule_frame);
    void copy_frame(wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                    wlr_ext_image_capture_source_v1_frame_event *frame_event);
    wlr_ext_image_capture_source_v1_cursor *get_pointer_cursor(wlr_seat *seat);
    static void start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors);
    static void stop(struct wlr_ext_image_capture_source_v1 *source);
    static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule_frame);
    static void copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                           wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                           wlr_ext_image_capture_source_v1_frame_event *frame_event);
    static wlr_ext_image_capture_source_v1_cursor *get_pointer_cursor(
        struct wlr_ext_image_capture_source_v1 *source, struct wlr_seat *seat);

private Q_SLOTS:
    void updateGeometry();
    void updateConstraints();
    // Runs inside the compositor's natural frame (render window beforeRendering),
    // renders a fresh snapshot into the private buffer and announces it.
    void renderSnapshot();

private:
    WOutputRenderWindow *renderWindow() const;
    void announceFrame();
    QSize currentPixelSize() const;
    void startSnapshotPass();

    wlr_ext_image_capture_source_v1 source;

    QPointer<WSurfaceItem> m_surfaceItem;
    QPointer<WOutput> m_output;
    CaptureBufferRenderer *m_renderer = nullptr;
    QMetaObject::Connection m_beforeRenderingConnection;
    QMetaObject::Connection m_renderEndConnection;
    QRectF m_sourceRect;
    QSize m_pixelSize;
    bool m_capturing = false;
    // One snapshot per compositor frame: QQuickWindow::beforeRendering can be
    // emitted more than once per frame (waylib re-emits it after sync), so a
    // latch reset on renderEnd keeps one snapshot per natural frame.
    bool m_snapshotDoneThisFrame = false;
};

WAYLIB_SERVER_END_NAMESPACE