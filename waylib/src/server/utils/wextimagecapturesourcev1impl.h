// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wlr_fwd.h>
#include <wglobal.h>
#include <wlr_all.h>

#include <QObject>
#include <QPointer>

WAYLIB_SERVER_BEGIN_NAMESPACE

class WOutput;
class WOutputViewport;

// Implements a wlr_ext_image_capture_source_v1 on top of a WOutputViewport.
// The viewport renders the captured item subtree (e.g. a whole toplevel with
// its subsurfaces and decorations) into its own dedicated buffer, and this
// class forwards the viewport's buffers as capture frames.
class WAYLIB_SERVER_EXPORT WExtImageCaptureSourceV1Impl : public QObject
{
    Q_OBJECT
public:
    explicit WExtImageCaptureSourceV1Impl(WOutputViewport *viewport, QObject *parent = nullptr);
    ~WExtImageCaptureSourceV1Impl();

    wlr_ext_image_capture_source_v1 *handle() { return &source; }

private:
    static const struct wlr_ext_image_capture_source_v1_interface impl;
    QSize currentPixelSize() const;
    void updateConstraints();
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
    void handleRenderEnd();

private:
    wlr_ext_image_capture_source_v1 source;

    QPointer<WOutputViewport> m_viewport;
    WOutput *m_output;
    bool m_capturing;
    QMetaObject::Connection m_renderEndConnection;
};

WAYLIB_SERVER_END_NAMESPACE
