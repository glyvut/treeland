// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wextimagecapturesourcev1impl.h"
#include <wpointer.h>
#include "wcursor.h"
#include "wcursorimage.h"
#include "wimagebuffer.h"
#include "wseat.h"
#include "wsurface.h"
#include "wsurfaceitem.h"
#include "wsgtextureprovider.h"
#include "woutputrenderwindow.h"
#include "woutput.h"
#include "wtools.h"
#include "wayliblogging.h"
#include "wbufferrenderer_p.h"

#include <wlr_all.h>
#include <wcontainerof.h>

extern "C" {
#include <pixman.h>
#include <drm_fourcc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
}

WAYLIB_SERVER_BEGIN_NAMESPACE

// The wlr_ext_image_capture_source_v1 has no data field and
// WExtImageCaptureSourceV1Impl is not standard-layout (QObject base), so a
// container_of lookup is not possible; use a registry instead.
static QHash<wlr_ext_image_capture_source_v1 *, WExtImageCaptureSourceV1Impl *> s_captureSourceMap;

// Pointer-cursor capture source for one seat, created lazily by
// WExtImageCaptureSourceV1Impl::get_pointer_cursor(). It feeds the
// ext_image_copy_capture_manager_v1.create_pointer_cursor_session() protocol:
// the compositor reports cursor enter/leave/position/hotspot through the
// embedded wlr_ext_image_capture_source_v1_cursor and produces the cursor
// image in copy_frame().
struct WImageCaptureCursorSource
{
    // The cursor's base (wlr_ext_image_capture_source_v1) is also the target
    // for the impl callbacks, so keep a registry for the reverse lookup.
    wlr_ext_image_capture_source_v1_cursor cursor;

    wlr_seat *seat = nullptr;
    WOutput *output = nullptr;
    QQuickItem *surfaceItem = nullptr;
    QPointer<WOutputRenderWindow> renderWindow;

    // Rasterized cursor (compositor/bitmap cursor path). `image` is stored in
    // device pixels; `buffer` is the wlr buffer wrapping it.
    WCursorImage *cursorImage = nullptr;
    QImage image;
    QPoint hotspot;
    WBufferDropPtr buffer;

    // Client-provided cursor surface (wl_pointer.set_cursor) path. When set,
    // its current buffer is copied directly instead of `image`.
    QPointer<WSurface> cursorSurface;

    // Frame events must be emitted on the render thread (the copy_frame
    // implementation performs GL operations), so pending frames are deferred
    // to the next WOutputRenderWindow::renderEnd.
    bool framePending = false;

    QMetaObject::Connection positionConnection;
    QMetaObject::Connection visibleConnection;
    QMetaObject::Connection cursorChangedConnection;
    QMetaObject::Connection shapeChangedConnection;
    QMetaObject::Connection surfaceChangedConnection;
    QMetaObject::Connection commitConnection;
    QMetaObject::Connection renderEndConnection;

    WImageCaptureCursorSource()
        : cursor{}
    {
        cursorImage = new WCursorImage;
    }

    ~WImageCaptureCursorSource()
    {
        disconnectConnections();
        // Emits destroy (tearing down any live cursor sessions) and releases
        // the base's shm/dmabuf formats.
        wlr_ext_image_capture_source_v1_cursor_finish(&cursor);
        delete cursorImage;
    }

    void disconnectConnections()
    {
        if (positionConnection)
            QObject::disconnect(positionConnection);
        if (visibleConnection)
            QObject::disconnect(visibleConnection);
        if (cursorChangedConnection)
            QObject::disconnect(cursorChangedConnection);
        if (shapeChangedConnection)
            QObject::disconnect(shapeChangedConnection);
        if (surfaceChangedConnection)
            QObject::disconnect(surfaceChangedConnection);
        if (commitConnection)
            QObject::disconnect(commitConnection);
        if (renderEndConnection)
            QObject::disconnect(renderEndConnection);
    }

    void connectCursor();
    void refreshImage();
    void updatePosition();
    void updateConstraints(int width, int height);
    void scheduleFrame();
    void emitFrame();
    void emitUpdate();

    static const struct wlr_ext_image_capture_source_v1_interface impl;
    static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule_frame);
    static void copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                           wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                           wlr_ext_image_capture_source_v1_frame_event *frame_event);
};

static QHash<wlr_ext_image_capture_source_v1 *, WImageCaptureCursorSource *> s_cursorSourceMap;

const struct wlr_ext_image_capture_source_v1_interface WImageCaptureCursorSource::impl = {
    .start = nullptr,
    .stop = nullptr,
    .request_frame = WImageCaptureCursorSource::request_frame,
    .copy_frame = WImageCaptureCursorSource::copy_frame,
    .get_pointer_cursor = nullptr,
};

// Helper for constraint building
struct ConstraintBuilder {
    wlr_ext_image_capture_source_v1 *source;
    WOutput *output;

    ConstraintBuilder(wlr_ext_image_capture_source_v1 *src, WOutput *out)
        : source(src), output(out) {}

    void setSize(int width, int height) {
        source->width = width;
        source->height = height;
    }

    void buildShmFormats() {
        auto renderer = output->renderer();
        auto swapchain = output->swapchain();
        uint32_t format = DRM_FORMAT_ARGB8888; // fallback

        if (renderer && swapchain) {
            if (struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain)) {
                WBufferUnlockPtr bufferGuard(buffer);
                WUniquePointer<wlr_texture> texture(
                    wlr_texture_from_buffer(renderer, buffer));

                if (texture) {
                    uint32_t shm_format = wlr_texture_preferred_read_format(texture.get());
                    if (shm_format != DRM_FORMAT_INVALID) {
                        format = shm_format;
                    }
                }
            }
        }

        // wlroots frees shm_formats with free() (ext_image_capture_source_v1),
        // so the allocation must use the C allocator — new[]/delete[] would be
        // an alloc/dealloc mismatch (UB, ASan alloc-dealloc-mismatch).
        free(source->shm_formats);
        source->shm_formats = static_cast<uint32_t*>(malloc(sizeof(uint32_t)));
        if (source->shm_formats) {
            source->shm_formats[0] = format;
            source->shm_formats_len = 1;
        } else {
            source->shm_formats_len = 0;
        }

        qCDebug(lcWlImageCapture) << "Set SHM format:" << format;
    }

    void buildDmabufFormats() {
        auto renderer = output->renderer();
        auto swapchain = output->swapchain();

        if (!renderer || !swapchain) return;

        int drm_fd = wlr_renderer_get_drm_fd(renderer);
        if (swapchain->allocator &&
            (swapchain->allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF) &&
            drm_fd >= 0) {

            struct stat dev_stat;
            if (fstat(drm_fd, &dev_stat) == 0) {
                source->dmabuf_device = dev_stat.st_rdev;

                // Clean up old DMA-BUF formats
                wlr_drm_format_set_finish(&source->dmabuf_formats);
                source->dmabuf_formats = (struct wlr_drm_format_set){};

                // Copy DMA-BUF formats from swapchain
                for (size_t i = 0; i < swapchain->format.len; i++) {
                    wlr_drm_format_set_add(&source->dmabuf_formats,
                        swapchain->format.format, swapchain->format.modifiers[i]);
                }
                qCDebug(lcWlImageCapture) << "Set DMA-BUF constraints";
            }
        }
    }

    void apply() {
        wl_signal_emit_mutable(&source->events.constraints_update, nullptr);
    }
};

const struct wlr_ext_image_capture_source_v1_interface WExtImageCaptureSourceV1Impl::impl = {
    .start = WExtImageCaptureSourceV1Impl::start,
    .stop = WExtImageCaptureSourceV1Impl::stop,
    .request_frame = WExtImageCaptureSourceV1Impl::request_frame,
    .copy_frame = WExtImageCaptureSourceV1Impl::copy_frame,
    .get_pointer_cursor = WExtImageCaptureSourceV1Impl::get_pointer_cursor,
};

WExtImageCaptureSourceV1Impl::WExtImageCaptureSourceV1Impl(QQuickItem *surfaceItem, WOutput *output)
    : QObject(surfaceItem)
    , m_surfaceItem(surfaceItem)
    , m_output(output)
    , m_capturing(false)
{
    Q_ASSERT(m_surfaceItem);
    Q_ASSERT(m_output);

    // Initialize wlr_ext_image_capture_source_v1
    wlr_ext_image_capture_source_v1_init(&source, &impl);
    s_captureSourceMap.insert(&source, this);

    // Set initial constraints from the surfaceItem's bounding rect * dpr
    const auto pixelSize = computePixelSize();
    if (pixelSize.isValid() && !pixelSize.isEmpty()) {
        ConstraintBuilder builder(&source, m_output);
        builder.setSize(pixelSize.width(), pixelSize.height());
        builder.buildShmFormats();
        builder.buildDmabufFormats();
        builder.apply();

        qCDebug(lcWlImageCapture) << "Initial constraints set:" << pixelSize;
    } else {
        qCWarning(lcWlImageCapture) << "Invalid surface dimensions for constraints:" << pixelSize;
    }
}

WExtImageCaptureSourceV1Impl::~WExtImageCaptureSourceV1Impl()
{
    if (m_capturing) {
        stop();
    }
    delete m_captureRenderer;

    for (auto *cs : std::as_const(m_cursorSources)) {
        s_cursorSourceMap.remove(&cs->cursor.base);
        delete cs;
    }
    m_cursorSources.clear();

    wlr_ext_image_capture_source_v1_finish(&source);
    s_captureSourceMap.remove(&source);
}

WOutputRenderWindow *WExtImageCaptureSourceV1Impl::renderWindow() const
{
    if (!m_surfaceItem)
        return nullptr;
    return qobject_cast<WOutputRenderWindow *>(m_surfaceItem->window());
}

qreal WExtImageCaptureSourceV1Impl::computeDpr() const
{
    auto rw = renderWindow();
    return rw ? rw->effectiveDevicePixelRatio() : 1.0;
}

QSize WExtImageCaptureSourceV1Impl::computePixelSize() const
{
    if (!m_surfaceItem)
        return { };

    const auto sz = m_surfaceItem->size();
    if (sz.isEmpty())
        return { };

    const qreal dpr = computeDpr();
    return QSize(qCeil(sz.width() * dpr), qCeil(sz.height() * dpr));
}

void WExtImageCaptureSourceV1Impl::updateConstraints(const QSize &pixelSize)
{
    if (!pixelSize.isValid() || pixelSize.isEmpty())
        return;

    ConstraintBuilder builder(handle(), m_output);
    builder.setSize(pixelSize.width(), pixelSize.height());
    builder.buildShmFormats();
    builder.buildDmabufFormats();
    builder.apply();
}

void WExtImageCaptureSourceV1Impl::start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    self->start(with_cursors);
}

void WExtImageCaptureSourceV1Impl::start(bool with_cursors)
{
    m_capturing = true;
    qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl::start() with_cursors:" << with_cursors;

    // TODO: Optimize multiple clients capturing the same window
    // Currently each client creates its own WExtImageCaptureSourceV1Impl instance,
    // which means multiple render listeners for the same surface. Consider implementing
    // a manager to share render events among multiple capture sources.

    auto rw = renderWindow();
    if (!rw) {
        qCWarning(lcWlImageCapture) << "No render window available for start";
        return;
    }

    if (!m_captureRenderer) {
        m_captureRenderer = new WBufferRenderer(rw->contentItem());
        m_captureRenderer->setOutput(m_output);
        m_captureRenderer->setVisible(false);
    }

    m_afterRenderingConnection = connect(rw,
                                         &QQuickWindow::afterRendering,
                                         this,
                                         &WExtImageCaptureSourceV1Impl::doOffscreenRender,
                                         Qt::AutoConnection);

    m_renderEndConnection = connect(rw,
                                    &WOutputRenderWindow::renderEnd,
                                    this,
                                    &WExtImageCaptureSourceV1Impl::handleRenderEnd,
                                    Qt::AutoConnection);

    // Trigger first frame
    wlr_output_update_needs_frame(m_output->handle());
}

void WExtImageCaptureSourceV1Impl::stop(struct wlr_ext_image_capture_source_v1 *source)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    self->stop();
}

void WExtImageCaptureSourceV1Impl::stop()
{
    m_capturing = false;
    qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl::stop()";

    if (m_afterRenderingConnection) {
        disconnect(m_afterRenderingConnection);
        m_afterRenderingConnection = QMetaObject::Connection();
    }
    if (m_renderEndConnection) {
        disconnect(m_renderEndConnection);
        m_renderEndConnection = QMetaObject::Connection();
    }

    if (m_renderedBuffer) {
        wlr_buffer_unlock(m_renderedBuffer);
    }
    m_renderedBuffer = nullptr;
}

void WExtImageCaptureSourceV1Impl::request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule_frame)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    self->schedule_frame(schedule_frame);
}

void WExtImageCaptureSourceV1Impl::schedule_frame([[maybe_unused]] bool schedule_frame)
{
    qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl::schedule_frame()";

    if (!m_capturing) {
        qCWarning(lcWlImageCapture) << "schedule_frame called but not capturing";
        return;
    }

    // Request output update to ensure next frame will be rendered.
    // doOffscreenRender fires via afterRendering, handleRenderEnd via renderEnd.
    wlr_output_update_needs_frame(m_output->handle());
}

void WExtImageCaptureSourceV1Impl::doOffscreenRender()
{
    if (!m_capturing || !m_surfaceItem || !m_captureRenderer)
        return;

    auto rw = renderWindow();
    if (!rw)
        return;

    const auto pixelSize = computePixelSize();
    if (pixelSize.isEmpty()) {
        qCWarning(lcWlImageCapture) << "Invalid pixel size for offscreen render:" << pixelSize;
        return;
    }

    const qreal dpr = computeDpr();

    if (m_renderedBuffer) {
        wlr_buffer_unlock(m_renderedBuffer);
        m_renderedBuffer = nullptr;
    }

    // Render the surface item's subtree (content + subsurfaces) into an
    // offscreen FBO via transient root-node re-parenting.  No layer.enabled
    // on m_surfaceItem — avoids the double-resampling blur.
    m_renderedBuffer = rw->renderItemToBuffer(m_captureRenderer,
                                              m_surfaceItem,
                                              pixelSize,
                                              dpr,
                                              DRM_FORMAT_ARGB8888);

    if (m_renderedBuffer) {
        // Lock to prevent swapchain from recycling before copy_frame
        wlr_buffer_lock(m_renderedBuffer);
    }

    qCDebug(lcWlImageCapture) << "Offscreen render done, buffer:"
                              << (m_renderedBuffer ? m_renderedBuffer.get() : nullptr)
                              << "size:" << pixelSize;
}

void WExtImageCaptureSourceV1Impl::handleRenderEnd()
{
    if (!m_capturing) {
        qCWarning(lcWlImageCapture) << "handleRenderEnd called but not capturing";
        return;
    }

    if (!m_renderedBuffer) {
        qCWarning(lcWlImageCapture) << "No rendered buffer available for frame event";
        return;
    }

    auto wlr_buf = m_renderedBuffer.get();
    const int bufferWidth = wlr_buf->width;
    const int bufferHeight = wlr_buf->height;
    if (bufferWidth <= 0 || bufferHeight <= 0) {
        qCWarning(lcWlImageCapture) << "Invalid buffer size:" << bufferWidth << "x" << bufferHeight;
        return;
    }

    // TODO: partial damage
    WPixmanRegion fullDamage(0, 0, bufferWidth, bufferHeight);

    wlr_ext_image_capture_source_v1_frame_event event {
        .damage = fullDamage.get(),
    };
    wl_signal_emit_mutable(&source.events.frame, &event);

    qCDebug(lcWlImageCapture) << "Frame event emitted with damage:" << bufferWidth << "x"
                              << bufferHeight;
}

void WExtImageCaptureSourceV1Impl::copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                                              wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                                              wlr_ext_image_capture_source_v1_frame_event *frame_event)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    self->copy_frame(dst_frame, frame_event);
}

void WExtImageCaptureSourceV1Impl::copy_frame(
    wlr_ext_image_copy_capture_frame_v1 *dst_frame,
    [[maybe_unused]] wlr_ext_image_capture_source_v1_frame_event *frame_event)
{
    qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl::copy_frame()";

    if (!m_capturing) {
        qCWarning(lcWlImageCapture) << "copy_frame called but not capturing";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }

    if (!m_renderedBuffer) {
        qCWarning(lcWlImageCapture) << "No rendered buffer available for copy";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    auto renderer = m_output->renderer();
    if (!renderer) {
        qCWarning(lcWlImageCapture) << "No renderer available";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    auto src = m_renderedBuffer.get();
    // Buffer is already locked in doOffscreenRender; unlock after copy.
    // copy_buffer calls fail() + frame_destroy + free internally on failure,
    // so do NOT touch dst_frame afterwards.

    if (!dst_frame || !dst_frame->buffer) {
        qCWarning(lcWlImageCapture) << "Destination frame or buffer is null";
        if (dst_frame) {
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        }
        wlr_buffer_unlock(src);
        m_renderedBuffer = nullptr;
        return;
    }

    // Validate buffer dimensions
    if (dst_frame->buffer->width != src->width || dst_frame->buffer->height != src->height) {
        qCWarning(lcWlImageCapture) << "Buffer size mismatch (dst:" << dst_frame->buffer->width
                                    << "x" << dst_frame->buffer->height << ", src:" << src->width
                                    << "x" << src->height << "), updating constraints";

        updateConstraints(QSize(src->width, src->height));

        if (dst_frame->buffer->width != src->width || dst_frame->buffer->height != src->height) {
            qCDebug(lcWlImageCapture) << "Buffer size still mismatched after constraint update";
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
            wlr_buffer_unlock(src);
            m_renderedBuffer = nullptr;
            return;
        }
    }

    bool success = wlr_ext_image_copy_capture_frame_v1_copy_buffer(dst_frame, src, renderer);
    qCDebug(lcWlImageCapture) << "Copy result:" << success;

    // Unlock regardless of success/failure.
    wlr_buffer_unlock(src);
    m_renderedBuffer = nullptr;

    if (success) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        wlr_ext_image_copy_capture_frame_v1_ready(dst_frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
        qCDebug(lcWlImageCapture) << "Frame copy successful";
    } else {
        qCWarning(lcWlImageCapture) << "Failed to copy frame buffer";
    }
}

wlr_ext_image_capture_source_v1_cursor *WExtImageCaptureSourceV1Impl::get_pointer_cursor(
    struct wlr_ext_image_capture_source_v1 *source, struct wlr_seat *seat)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    return self->get_pointer_cursor(seat);
}

void WImageCaptureCursorSource::connectCursor()
{
    auto *wseat = WSeat::fromHandle(seat);
    auto *wcursor = wseat ? wseat->cursor() : nullptr;

    if (output)
        cursorImage->setScale(output->scale());

    if (renderWindow) {
        // Deliver pending frames on the render thread, where copy_frame can
        // safely create textures / run GL operations.
        renderEndConnection = QObject::connect(renderWindow, &WOutputRenderWindow::renderEnd,
                                               [this] {
            if (!framePending)
                return;
            framePending = false;
            emitFrame();
        });
    }

    if (!wcursor)
        return;

    positionConnection = QObject::connect(wcursor, &WCursor::positionChanged, [this] {
        updatePosition();
    });
    visibleConnection = QObject::connect(wcursor, &WCursor::visibleChanged, [this] {
        updatePosition();
    });
    cursorChangedConnection = QObject::connect(wcursor, &WCursor::cursorChanged, [this] {
        refreshImage();
    });
    shapeChangedConnection = QObject::connect(wcursor, &WCursor::requestedCursorShapeChanged, [this] {
        refreshImage();
    });
    surfaceChangedConnection = QObject::connect(wcursor, &WCursor::requestedCursorSurfaceChanged, [this] {
        refreshImage();
    });
}

void WImageCaptureCursorSource::refreshImage()
{
    auto *wseat = WSeat::fromHandle(seat);
    auto *wcursor = wseat ? wseat->cursor() : nullptr;

    WSurface *newCursorSurface = nullptr;
    QImage newImage;
    QPoint newHotspot;
    int32_t newHotX = 0;
    int32_t newHotY = 0;

    if (wcursor) {
        const QCursor qc = wcursor->cursor();
        if (WGlobal::isClientResourceCursor(qc)) {
            // Client-set cursor: prefer the cursor surface, fall back to a
            // cursor shape.
            const auto res = wcursor->requestedCursorSurface();
            if (auto *surf = res.first; surf && surf->buffer()) {
                newCursorSurface = surf;
                const int scale = surf->bufferScale();
                newHotX = res.second.x() * scale;
                newHotY = res.second.y() * scale;
            } else {
                const auto shape = wcursor->requestedCursorShape();
                if (shape != WGlobal::CursorShape::Invalid) {
                    cursorImage->setCursor(WCursor::toQCursor(shape));
                    newImage = cursorImage->image();
                    newHotspot = cursorImage->hotSpot();
                    newHotX = newHotspot.x();
                    newHotY = newHotspot.y();
                }
            }
        } else if (!WGlobal::isInvalidCursor(qc)) {
            cursorImage->setCursor(qc);
            newImage = cursorImage->image();
            newHotspot = cursorImage->hotSpot();
            newHotX = newHotspot.x();
            newHotY = newHotspot.y();
        }
    }

    const bool surfaceChanged = cursorSurface != newCursorSurface;
    cursorSurface = newCursorSurface;

    const bool imageChanged = newImage.isNull() != image.isNull()
                              || newImage.size() != image.size()
                              || (!image.isNull() && newImage != image);
    image = newImage;
    hotspot = newHotspot;

    if (surfaceChanged) {
        if (commitConnection)
            QObject::disconnect(commitConnection);
        commitConnection = {};
        if (cursorSurface) {
            // A new cursor buffer may arrive on the next commit.
            commitConnection = QObject::connect(cursorSurface, &WSurface::commit, [this] {
                refreshImage();
            });
        }
    }

    const bool hotChanged = cursor.hotspot.x != newHotX || cursor.hotspot.y != newHotY;
    cursor.hotspot.x = newHotX;
    cursor.hotspot.y = newHotY;

    // Regenerate the rasterized buffer whenever the image content changed.
    if (imageChanged) {
        buffer.reset();
        if (!image.isNull())
            buffer.reset(WImageBufferImpl::create(image));
    }

    // Refresh the constraints and notify clients about the new content.
    const int pixelWidth = image.isNull()
        ? (cursorSurface && cursorSurface->buffer() ? cursorSurface->buffer()->width : 0)
        : image.width();
    const int pixelHeight = image.isNull()
        ? (cursorSurface && cursorSurface->buffer() ? cursorSurface->buffer()->height : 0)
        : image.height();
    if (pixelWidth > 0 && pixelHeight > 0
        && (static_cast<int>(cursor.base.width) != pixelWidth
            || static_cast<int>(cursor.base.height) != pixelHeight)) {
        updateConstraints(pixelWidth, pixelHeight);
    }

    if (surfaceChanged || imageChanged) {
        scheduleFrame();
    }
    if (hotChanged || surfaceChanged || imageChanged) {
        emitUpdate();
    }
}

void WImageCaptureCursorSource::updatePosition()
{
    auto *wseat = WSeat::fromHandle(seat);
    auto *wcursor = wseat ? wseat->cursor() : nullptr;
    const bool visible = wcursor && wcursor->isVisible();
    const QPointF globalPos = wcursor ? wcursor->position() : QPointF();

    bool newEntered = false;
    int32_t newX = 0;
    int32_t newY = 0;
    if (visible && surfaceItem) {
        // The surface item lives in the render window scene, which maps 1:1 to
        // the output layout coordinate space of WCursor::position().
        const QPointF origin = surfaceItem->mapToScene(QPointF(0, 0));
        const QPointF rel = globalPos - origin;
        newX = qRound(rel.x());
        newY = qRound(rel.y());
        newEntered = QRectF(QPointF(0, 0), surfaceItem->size()).contains(rel);
    }

    const bool enteredChanged = cursor.entered != newEntered;
    const bool stateChanged = enteredChanged || cursor.x != newX || cursor.y != newY;
    cursor.entered = newEntered;
    cursor.x = newX;
    cursor.y = newY;

    if (stateChanged) {
        emitUpdate();
        // The cursor just entered the surface: make sure an in-flight capture
        // has a frame to deliver.
        if (enteredChanged && newEntered)
            scheduleFrame();
    }
}

void WImageCaptureCursorSource::updateConstraints(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    ConstraintBuilder builder(&cursor.base, output);
    builder.setSize(width, height);
    builder.buildShmFormats();
    builder.buildDmabufFormats();
    builder.apply();
}

void WImageCaptureCursorSource::scheduleFrame()
{
    if (framePending)
        return;
    framePending = true;
    if (output)
        wlr_output_update_needs_frame(output->handle());
}

void WImageCaptureCursorSource::emitFrame()
{
    if (cursor.base.width <= 0 || cursor.base.height <= 0)
        return;
    WPixmanRegion full(0, 0, cursor.base.width, cursor.base.height);
    wlr_ext_image_capture_source_v1_frame_event event {
        .damage = full.get(),
    };
    wl_signal_emit_mutable(&cursor.base.events.frame, &event);
}

void WImageCaptureCursorSource::emitUpdate()
{
    wl_signal_emit_mutable(&cursor.events.update, nullptr);
}

void WImageCaptureCursorSource::request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule_frame)
{
    auto *self = s_cursorSourceMap.value(source);
    if (!self)
        return;
    if (!schedule_frame)
        return;
    // The cursor image can be produced on the next render pass; request it.
    self->scheduleFrame();
}

void WImageCaptureCursorSource::copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                                           wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                                           [[maybe_unused]] wlr_ext_image_capture_source_v1_frame_event *frame_event)
{
    auto *self = s_cursorSourceMap.value(source);
    if (!self) {
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }

    auto renderer = self->output ? self->output->renderer() : nullptr;
    if (!renderer) {
        qCWarning(lcWlImageCapture) << "WImageCaptureCursorSource: no renderer available";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    wlr_buffer *src = nullptr;
    bool surfacePath = false;
    if (self->cursorSurface && self->cursorSurface->buffer()) {
        src = self->cursorSurface->buffer();
        surfacePath = true;
        wlr_buffer_lock(src);
    } else if (self->buffer) {
        src = self->buffer.get();
    }

    if (!src) {
        qCDebug(lcWlImageCapture) << "WImageCaptureCursorSource: no cursor image available";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }

    // Buffer size mismatch: update constraints and let the client retry.
    if (static_cast<int>(source->width) != src->width
        || static_cast<int>(source->height) != src->height) {
        self->updateConstraints(src->width, src->height);
        if (static_cast<int>(source->width) != src->width
            || static_cast<int>(source->height) != src->height) {
            if (surfacePath)
                wlr_buffer_unlock(src);
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame,
                EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
            return;
        }
    }

    const bool ok = wlr_ext_image_copy_capture_frame_v1_copy_buffer(dst_frame, src, renderer);
    if (surfacePath)
        wlr_buffer_unlock(src);

    if (!ok)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_ext_image_copy_capture_frame_v1_ready(dst_frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
    qCDebug(lcWlImageCapture) << "WImageCaptureCursorSource: cursor frame copied";
}

wlr_ext_image_capture_source_v1_cursor *WExtImageCaptureSourceV1Impl::get_pointer_cursor(wlr_seat *seat)
{
    if (!seat)
        return nullptr;

    if (auto *existing = m_cursorSources.value(seat))
        return &existing->cursor;

    auto *cs = new WImageCaptureCursorSource;
    cs->seat = seat;
    cs->output = m_output;
    cs->surfaceItem = m_surfaceItem.get();
    cs->renderWindow = renderWindow();

    wlr_ext_image_capture_source_v1_cursor_init(&cs->cursor, &WImageCaptureCursorSource::impl);
    s_cursorSourceMap.insert(&cs->cursor.base, cs);
    m_cursorSources.insert(seat, cs);

    cs->connectCursor();
    cs->updatePosition();
    cs->refreshImage();

    qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl::get_pointer_cursor() created for seat" << seat;
    return &cs->cursor;
}

WAYLIB_SERVER_END_NAMESPACE
