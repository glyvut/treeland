// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wlr_fwd.h>
#include <wglobal.h>
#include <wlr_all.h>

#include <QHash>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QSet>

QT_BEGIN_NAMESPACE
class QQuickItem;
QT_END_NAMESPACE

WAYLIB_SERVER_BEGIN_NAMESPACE

class WBufferRenderer;
class WOutputRenderWindow;
class WScopedListener;
struct SourceResourceDestroyListener;

class WAYLIB_SERVER_EXPORT WExtImageCaptureSourceV1Impl : public QObject
{
    Q_OBJECT
public:
    explicit WExtImageCaptureSourceV1Impl(QObject *parent = nullptr);
    ~WExtImageCaptureSourceV1Impl() override;

    wlr_ext_image_capture_source_v1 *handle() { return &source; }

    // Accepts a foreign-toplevel capture request and tracks the client
    // resource, so the source can be recycled once the client destroys it.
    bool accept(wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request);

    // Connect the copy-capture manager's new_session event to this callback.
    static void trackSession(wlr_ext_image_copy_capture_session_v1 *session);

public Q_SLOTS:
    void invalidate();

protected:
    // Return a private item tree in the compositor's render window.
    virtual QQuickItem *createCaptureContent() = 0;
    void setCaptureGeometry(const QRectF &sourceRect, const QSize &pixelSize, qreal dpr);
    void handleContentChanged();

private:
    static const struct wlr_ext_image_capture_source_v1_interface impl;
    void start(bool with_cursors);
    void stop();
    void requestFrame(bool scheduleFrame);
    void frameDestroyed(wlr_ext_image_copy_capture_frame_v1 *frame);
    bool needsRender() const;
    void handleSceneGraphChanged();
    void beginFrame();
    void sendFrame(wlr_buffer *buffer, bool damaged);
    void copy_frame(wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                    wlr_ext_image_capture_source_v1_frame_event *frame_event);
    static void start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors);
    static void stop(struct wlr_ext_image_capture_source_v1 *source);
    static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule_frame);
    static void copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                           wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                           wlr_ext_image_capture_source_v1_frame_event *frame_event);
    static wlr_ext_image_capture_source_v1_cursor *get_pointer_cursor(
        struct wlr_ext_image_capture_source_v1 *source, struct wlr_seat *seat);

    bool createPipeline();
    void releasePipeline();
    void configureTarget();
    void attachResourceListeners();
    void detachResourceListeners();
    void scheduleRecycleCheck();
    static void resourceDestroyed(wl_listener *listener, void *data);

private Q_SLOTS:
    void maybeRecycle();
    void handleRenderFinished(wlr_buffer *buffer);
    void updateConstraints();
    void schedulePendingRender();

private:
    wlr_ext_image_capture_source_v1 source;

    QPointer<QQuickItem> m_content;
    QPointer<WBufferRenderer> m_renderer;
    QPointer<WOutputRenderWindow> m_renderWindow;
    QList<QMetaObject::Connection> m_pipelineConnections;
    QMetaObject::Connection m_constraintsUpdate;
    QRectF m_sourceRect;
    QSize m_pixelSize;
    qreal m_devicePixelRatio = 1.0;
    QHash<wl_resource *, SourceResourceDestroyListener *> m_resourceListeners;
    QHash<wlr_ext_image_copy_capture_session_v1 *, WScopedListener *> m_sessionListeners;
    QHash<wlr_ext_image_copy_capture_frame_v1 *, WScopedListener *> m_frameListeners;
    QSet<wlr_ext_image_copy_capture_frame_v1 *> m_forceFrames;
    size_t m_numStarted = 0;
    quint64 m_contentRevision = 1;
    quint64 m_renderingRevision = 0;
    quint64 m_renderedRevision = 0;
    bool m_constraintsFromSwapchain = false;
    bool m_finished = false;
    bool m_recycleCheckQueued = false;
    bool m_inDestructor = false;
};

WAYLIB_SERVER_END_NAMESPACE
