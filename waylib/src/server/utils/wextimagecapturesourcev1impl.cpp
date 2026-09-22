// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wextimagecapturesourcev1impl.h"

#include <wpointer.h>
#include "wsurfaceitem.h"
#include "wsgtextureprovider.h"
#include "woutputrenderwindow.h"
#include "woutput.h"
#include "wtools.h"
#include "wayliblogging.h"

#include "../qtquick/private/wbufferrenderer_p.h"

#include <wlr_all.h>
#include <wcontainerof.h>

#include <memory>

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

// Exposes the rendering entry points of WBufferRenderer (protected in the base
// class) for capture use.  The renderer is a standalone item: it is only
// parented into the render window to acquire the scene-graph context and is
// never attached to an output helper, so it never affects the physical output
// frame loop.
class CaptureBufferRenderer : public WBufferRenderer
{
public:
    using WBufferRenderer::WBufferRenderer;
    using WBufferRenderer::beginRender;
    using WBufferRenderer::endRender;
    using WBufferRenderer::render;
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

// Watches the lifetime of the capturing client. wlroots only unlinks the
// source wl_resource when the client destroys it (no event), so client
// disconnection is the only reclamation hook available besides our own
// destruction; without it the renderer/scene-graph resources would leak for
// every capture source a short-lived client ever created.
struct ClientDestroyGuard
{
    wl_listener listener;
    QPointer<WExtImageCaptureSourceV1Impl> impl;
};

namespace {

void clientDestroyNotify(wl_listener *listener, void *data)
{
    (void)data;
    ClientDestroyGuard *guard =
        wl_container_of(listener, guard, listener);
    wl_list_remove(&guard->listener.link);
    wl_list_init(&guard->listener.link);
    WExtImageCaptureSourceV1Impl *impl = guard->impl.data();
    delete guard;
    if (impl)
        impl->deleteLater();
}

}

WExtImageCaptureSourceV1Impl::WExtImageCaptureSourceV1Impl(WSurfaceItem *surfaceItem,
                                                           WOutput *output,
                                                           wl_client *client)
    : QObject(surfaceItem)
    , m_surfaceItem(surfaceItem)
    , m_output(output)
{
    Q_ASSERT(surfaceItem);
    Q_ASSERT(output);

    // Initialize wlr_ext_image_capture_source_v1
    wlr_ext_image_capture_source_v1_init(&source, &impl);
    s_captureSourceMap.insert(&source, this);

    if (client) {
        auto *guard = new ClientDestroyGuard{ {}, this };
        guard->listener.notify = clientDestroyNotify;
        wl_client_add_destroy_listener(client, &guard->listener);
    }

    // The snapshot renderer: a standalone item inside the render window's
    // scene, never attached to an output helper (no OutputHelper is created,
    // no wlr_output is ever touched). hideSource=false keeps the window
    // visible on screen during capture; cacheBuffer makes the texture
    // provider track every rendered buffer for copy_frame.
    if (auto rw = renderWindow()) {
        m_renderer = new CaptureBufferRenderer(rw->contentItem());
        m_renderer->setObjectName("toplevel-capture-renderer");
        m_renderer->setOutput(output);
        m_renderer->setSourceList({ surfaceItem }, false);
        m_renderer->setCacheBuffer(true);
    } else {
        qCWarning(lcWlImageCapture) << "No render window for toplevel capture; captures disabled";
    }

    connect(surfaceItem, &WSurfaceItem::boundingRectChanged,
            this, &WExtImageCaptureSourceV1Impl::updateGeometry);
    connect(output, &WOutput::scaleChanged,
            this, &WExtImageCaptureSourceV1Impl::updateGeometry);
    connect(surfaceItem, &QObject::destroyed,
            this, &WExtImageCaptureSourceV1Impl::deleteLater);

    updateGeometry();
}

WExtImageCaptureSourceV1Impl::~WExtImageCaptureSourceV1Impl()
{
    if (m_beforeRenderingConnection) {
        disconnect(m_beforeRenderingConnection);
        m_beforeRenderingConnection = {};
    }
    if (m_renderEndConnection) {
        disconnect(m_renderEndConnection);
        m_renderEndConnection = {};
    }
    if (m_capturing) {
        qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl destroyed while capturing";
    }
    if (m_renderer) {
        m_renderer->deleteLater();
        m_renderer = nullptr;
    }
    wlr_ext_image_capture_source_v1_finish(&source);
    s_captureSourceMap.remove(&source);
}

WOutputRenderWindow *WExtImageCaptureSourceV1Impl::renderWindow() const
{
    if (!m_surfaceItem)
        return nullptr;
    return qobject_cast<WOutputRenderWindow *>(m_surfaceItem->window());
}

QSize WExtImageCaptureSourceV1Impl::currentPixelSize() const
{
    if (m_pixelSize.isValid() && !m_pixelSize.isEmpty())
        return m_pixelSize;

    if (m_renderer) {
        if (auto tp = m_renderer->wTextureProvider()) {
            if (auto *buffer = tp->wlrBuffer())
                return QSize(buffer->width, buffer->height);
        }
    }

    return m_output ? m_output->size() : QSize();
}

void WExtImageCaptureSourceV1Impl::updateGeometry()
{
    if (!m_surfaceItem || !m_output)
        return;

    const QRectF bounds = m_surfaceItem->boundingRect();
    if (bounds.isEmpty()) {
        // Unmapped/minimized: keep the previous geometry; the next
        // boundingRectChanged will restore it.
        return;
    }

    const qreal scale = m_output->scale();
    const QSize pixelSize(qRound(bounds.width() * scale),
                          qRound(bounds.height() * scale));
    if (pixelSize == m_pixelSize && bounds == m_sourceRect)
        return;

    m_sourceRect = bounds;
    m_pixelSize = pixelSize;

    // Resize never touches the render window (no window->update(), no
    // schedule_frame, no needs_frame): the next natural compositor frame is
    // already expected because the resize was caused by a client commit, and
    // constraints_update lets the client prepare matching buffers.
    updateConstraints();
}

void WExtImageCaptureSourceV1Impl::updateConstraints()
{
    const QSize pixelSize = currentPixelSize();
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0) {
        qCWarning(lcWlImageCapture) << "Invalid pixel size for constraints:" << pixelSize;
        return;
    }

    ConstraintBuilder builder(&source, m_output);
    builder.setSize(pixelSize.width(), pixelSize.height());
    builder.buildShmFormats();
    builder.buildDmabufFormats();
    builder.apply();

    qCDebug(lcWlImageCapture) << "Constraints updated:"
                              << "  - Width:" << pixelSize.width()
                              << "  - Height:" << pixelSize.height();
}

void WExtImageCaptureSourceV1Impl::renderSnapshot()
{
    if (!m_capturing || !m_renderer || !m_surfaceItem)
        return;
    // Only one snapshot per compositor frame (beforeRendering can be
    // emitted multiple times within one frame).
    if (m_snapshotDoneThisFrame)
        return;

    const QSize pixelSize = currentPixelSize();
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0)
        return;

    // Already rendering inside the same frame (should not happen because the
    // connection is installed on start()).
    if (m_renderer->currentBuffer())
        return;

    const uint32_t format = m_output && m_output->handle()->render_format
        ? m_output->handle()->render_format
        : DRM_FORMAT_ARGB8888;

    wlr_buffer *buffer = m_renderer->beginRender(
        pixelSize, m_output->scale(), format,
        WBufferRenderer::DontConfigureSwapchain
            | WBufferRenderer::RedirectOpenGLContextDefaultFrameBufferObject);
    if (!buffer) {
        qCWarning(lcWlImageCapture) << "Snapshot beginRender failed";
        return;
    }

    // Explicit identity transform renders the input subtree in item-local
    // coordinates: the source rect (the window subtree bounds) is mapped
    // straight to the frame origin, so the window content is locked to the
    // capture frame regardless of its position in the scene.
    m_renderer->render(0, QMatrix4x4(), m_sourceRect,
                       QRectF(QPointF(0, 0), m_sourceRect.size()));
    m_renderer->endRender();

    m_snapshotDoneThisFrame = true;
    announceFrame();
}

void WExtImageCaptureSourceV1Impl::startSnapshotPass()
{
    // Frame boundary: allow the next natural frame to take a new snapshot.
    m_snapshotDoneThisFrame = false;
}

void WExtImageCaptureSourceV1Impl::announceFrame()
{
    if (!m_capturing)
        return;

    const QSize pixelSize = currentPixelSize();
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0)
        return;

    // Only announce frames whose snapshot buffer matches the advertised size;
    // right after a resize the old-size buffer is silently skipped and the
    // client keeps the previous frame until the new snapshot arrives.
    auto *tp = m_renderer ? m_renderer->wTextureProvider() : nullptr;
    if (!tp || !tp->wlrBuffer())
        return;
    if (tp->wlrBuffer()->width != pixelSize.width()
        || tp->wlrBuffer()->height != pixelSize.height()) {
        return;
    }

    WPixmanRegion fullDamage(0, 0, pixelSize.width(), pixelSize.height());
    wlr_ext_image_capture_source_v1_frame_event event {
        .damage = fullDamage.get(),
    };
    wl_signal_emit_mutable(&source.events.frame, &event);

    qCDebug(lcWlImageCapture) << "Frame event emitted with damage region:" << pixelSize;
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

    if (auto *rw = renderWindow()) {
        if (!m_beforeRenderingConnection) {
            // Produce a snapshot inside every natural compositor frame. The
            // render window is the only thing that may schedule frames, so a
            // capture can never force the physical output to repaint.
            m_beforeRenderingConnection = connect(
                rw, &QQuickWindow::beforeRendering,
                this, &WExtImageCaptureSourceV1Impl::renderSnapshot);
            m_renderEndConnection = connect(rw, &WOutputRenderWindow::renderEnd, this,
                    &WExtImageCaptureSourceV1Impl::startSnapshotPass);
        }
    }
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

    if (m_beforeRenderingConnection) {
        disconnect(m_beforeRenderingConnection);
        m_beforeRenderingConnection = {};
    }
    if (m_renderEndConnection) {
        disconnect(m_renderEndConnection);
        m_renderEndConnection = {};
    }
    m_snapshotDoneThisFrame = false;
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

    // The compositor's natural frame loop already renders a snapshot on every
    // frame while capturing; nothing needs to be scheduled here. In
    // particular no wlr_output_update_needs_frame() is ever called, so the
    // physical output is never forced to repaint by a capture client.
}

void WExtImageCaptureSourceV1Impl::copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                                              wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                                              wlr_ext_image_capture_source_v1_frame_event *frame_event)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    self->copy_frame(dst_frame, frame_event);
}

void WExtImageCaptureSourceV1Impl::copy_frame(wlr_ext_image_copy_capture_frame_v1 *dst_frame,
                                              [[maybe_unused]] wlr_ext_image_capture_source_v1_frame_event *frame_event)
{
    qCDebug(lcWlImageCapture) << "WExtImageCaptureSourceV1Impl::copy_frame()";

    if (!m_capturing) {
        qCWarning(lcWlImageCapture) << "copy_frame called but not capturing";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }

    if (!m_renderer) {
        qCWarning(lcWlImageCapture) << "No renderer available for frame copy";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    // Get the latest snapshot buffer
    auto textureProvider = m_renderer->wTextureProvider();
    if (!textureProvider) {
        qCWarning(lcWlImageCapture) << "No texture provider available";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    auto buffer = textureProvider->wlrBuffer();
    if (!buffer) {
        qCWarning(lcWlImageCapture) << "No snapshot buffer available yet";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    // Lock the buffer for the duration of the copy to prevent races during resize
    if (!wlr_buffer_lock(buffer)) {
        qCWarning(lcWlImageCapture) << "Failed to lock snapshot buffer";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    WBufferUnlockPtr bufferGuard(buffer);

    auto renderer = m_output ? m_output->renderer() : nullptr;
    if (!renderer) {
        qCWarning(lcWlImageCapture) << "No renderer available";
        wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    wlr_buffer *src = buffer;
    if (auto clientBuf = wlr_client_buffer_get(buffer)) {
        src = clientBuf->source;
    }

    if (!src) {
        qCWarning(lcWlImageCapture) << "Source buffer is null, cannot copy frame";
        if (dst_frame) {
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        }
        return;
    }

    if (!dst_frame || !dst_frame->buffer) {
        qCWarning(lcWlImageCapture) << "Destination frame or buffer is null, cannot copy";
        if (dst_frame) {
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        }
        return;
    }

    // Validate buffer dimensions to prevent crashes during resize
    if (dst_frame->buffer->width != src->width || dst_frame->buffer->height != src->height) {
        qCWarning(lcWlImageCapture) << "Buffer size mismatch (dst:" << dst_frame->buffer->width << "x" << dst_frame->buffer->height
                                    << ", src:" << src->width << "x" << src->height << "), updating constraints";

        updateConstraints();

        if (dst_frame->buffer->width != src->width || dst_frame->buffer->height != src->height) {
            qCDebug(lcWlImageCapture) << "Buffer size still mismatched after constraint update, skipping frame";
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
            return;
        }
    }

    bool success = wlr_ext_image_copy_capture_frame_v1_copy_buffer(dst_frame, src, renderer);
    qCDebug(lcWlImageCapture) << "Copy result:" << success;

    if (success) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        wlr_ext_image_copy_capture_frame_v1_ready(dst_frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
        qCDebug(lcWlImageCapture) << "Frame copy successful";
    } else {
        qCWarning(lcWlImageCapture) << "Failed to copy frame buffer";
        if (dst_frame->buffer && buffer
            && (dst_frame->buffer->width != buffer->width
                || dst_frame->buffer->height != buffer->height)) {
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        } else {
            wlr_ext_image_copy_capture_frame_v1_fail(dst_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        }
    }
}

wlr_ext_image_capture_source_v1_cursor *WExtImageCaptureSourceV1Impl::get_pointer_cursor(
    [[maybe_unused]] struct wlr_ext_image_capture_source_v1 *source, [[maybe_unused]] struct wlr_seat *seat)
{
    auto *self = s_captureSourceMap.value(source);
    Q_ASSERT(self);
    return self->get_pointer_cursor(seat);
}

wlr_ext_image_capture_source_v1_cursor *WExtImageCaptureSourceV1Impl::get_pointer_cursor([[maybe_unused]] wlr_seat *seat)
{
    // Cursor capture is not implemented; return nullptr to indicate no cursor.
    return nullptr;
}

WAYLIB_SERVER_END_NAMESPACE