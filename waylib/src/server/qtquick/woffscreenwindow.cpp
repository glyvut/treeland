// Copyright (C) 2024-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "woffscreenwindow.h"
#include "wbufferrenderer_p.h"
#include "wsgtextureprovider.h"
#include "woutputrenderwindow.h"
#include "wquicktextureproxy.h"

#include <private/qquickitem_p.h>

#include <drm_fourcc.h>

WAYLIB_SERVER_BEGIN_NAMESPACE

WOffscreenWindow::WOffscreenWindow(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, false);
}

WOffscreenWindow::~WOffscreenWindow()
{
    invalidate();
}

void WOffscreenWindow::invalidate()
{
    if (auto window = outputRenderWindow())
        window->detach(this);

    releaseProxy();
}

bool WOffscreenWindow::isTextureProvider() const
{
    return m_renderer && m_renderer->isTextureProvider();
}

QSGTextureProvider *WOffscreenWindow::textureProvider() const
{
    return m_renderer ? m_renderer->textureProvider() : nullptr;
}

WSGTextureProvider *WOffscreenWindow::wTextureProvider() const
{
    return m_renderer ? m_renderer->wTextureProvider() : nullptr;
}

WOutputRenderWindow *WOffscreenWindow::outputRenderWindow() const
{
    return qobject_cast<WOutputRenderWindow*>(window());
}

QQuickItem *WOffscreenWindow::source() const
{
    return m_source;
}

void WOffscreenWindow::setSource(QQuickItem *source)
{
    if (m_source == source)
        return;

    m_source = source;

    if (m_renderer)
        m_renderer->setSourceList({m_source.data()}, true);

    releaseProxy();
    ensureProxy();
    Q_EMIT sourceChanged();
}

WOutput *WOffscreenWindow::output() const
{
    return m_output;
}

void WOffscreenWindow::setOutput(WOutput *output)
{
    if (m_output == output)
        return;

    m_output = output;

    if (m_renderer)
        m_renderer->setOutput(m_output);
    else
        ensureRenderer();

    ensureProxy();

    Q_EMIT outputChanged();
}

qreal WOffscreenWindow::devicePixelRatio() const
{
    return m_devicePixelRatio;
}

void WOffscreenWindow::setDevicePixelRatio(qreal devicePixelRatio)
{
    if (qFuzzyCompare(m_devicePixelRatio, devicePixelRatio))
        return;

    m_devicePixelRatio = devicePixelRatio;
    Q_EMIT devicePixelRatioChanged();
}

QSize WOffscreenWindow::pixelSize() const
{
    if (!m_source || m_source->size().isEmpty())
        return {};

    return QSize(qCeil(m_source->width() * m_devicePixelRatio),
                 qCeil(m_source->height() * m_devicePixelRatio));
}

wlr_buffer *WOffscreenWindow::buffer() const
{
    return m_renderer ? m_renderer->lastBuffer() : nullptr;
}

void WOffscreenWindow::ensureRenderer()
{
    if (m_renderer || !m_output)
        return;

    m_renderer = new WBufferRenderer(this);
    m_renderer->setOutput(m_output);
    m_renderer->setCacheBuffer(true);

    // Render only the source subtree with a dedicated renderer. hideSource=true
    // refs the source out of the main content item render, so the two renderers
    // never touch the same scene graph nodes and corrupt each other's matrices.
    // The rendered result is composited back on screen by the texture proxy.
    if (m_source)
        m_renderer->setSourceList({m_source.data()}, true);
}

void WOffscreenWindow::ensureProxy()
{
    if (m_proxy || !m_source || !m_renderer)
        return;

    auto *parent = m_source->parentItem();
    if (!parent)
        return;

    m_proxy = new WQuickTextureProxy(parent);
    m_proxy->setSourceItem(m_renderer);
    QQuickItemPrivate::get(m_proxy)->anchors()->setFill(m_source);
    m_proxy->setZ(m_source->z());
    m_proxy->stackAfter(m_source);

    const QPointer<QQuickItem> sourceGuard = m_source;
    WQuickTextureProxy *proxy = m_proxy;
    connect(m_source, &QQuickItem::zChanged, m_proxy, [proxy, sourceGuard] {
        if (proxy && sourceGuard)
            proxy->setZ(sourceGuard->z());
    });
}

void WOffscreenWindow::releaseProxy()
{
    if (!m_proxy)
        return;

    m_proxy->setVisible(false);
    m_proxy->deleteLater();
    m_proxy = nullptr;
}

void WOffscreenWindow::renderContent()
{
    ensureRenderer();

    if (!m_renderer || !m_source || !m_output)
        return;

    auto *window = outputRenderWindow();
    if (!window)
        return;

    const QSizeF size = m_source->size();
    if (size.isEmpty())
        return;

    const QSize pixel = pixelSize();
    if (pixel.isEmpty())
        return;

    ensureProxy();

    // Keep the proxy in the source's stacking slot so the on-screen composite
    // preserves the window's z-order against other (non-captured) windows.
    if (m_proxy) {
        if (m_proxy->parentItem() != m_source->parentItem()) {
            releaseProxy();
            ensureProxy();
        } else {
            m_proxy->stackAfter(m_source);
        }
    }

    m_renderer->setSize(size);

    wlr_buffer *buffer = m_renderer->beginRender(pixel, m_devicePixelRatio,
                                                 DRM_FORMAT_ARGB8888,
                                                 WBufferRenderer::DontConfigureSwapchain);
    if (!buffer)
        return;

    window->pushRenderer(m_renderer);
    const QRectF rect(QPointF(0, 0), size);
    m_renderer->render(0, {}, rect, rect);
    m_renderer->endRender();
    window->clearRenderers();
}

void WOffscreenWindow::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);

    if (change == ItemSceneChange && data.window) {
        if (!qobject_cast<WOutputRenderWindow*>(data.window))
            qFatal() << "OffscreenWindow must using in OutputRenderWindow.";
    }
}

WAYLIB_SERVER_END_NAMESPACE

#include "moc_woffscreenwindow.cpp"
