// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "toplevelcapturecontent.h"

#include "core/qmlengine.h"
#include "core/waylandserver.h"
#include "output/output.h"
#include "surface/surfacewrapper.h"

#include <wsurfaceitem.h>
#include <woutputrenderwindow.h>
#include <QQuickRenderControl>
#include <private/qquickitem_p.h>
#include <private/qquickshadereffectsource_p.h>
#include <QtMath>

namespace {
class CaptureSurfaceObserver final : public QObject
{
public:
    CaptureSurfaceObserver(WSurfaceItem *item, ToplevelCaptureContent *content)
        : QObject(item), m_content(content)
    {
        connect(item->surface(), &WSurface::commit, this, &CaptureSurfaceObserver::committed);
        connect(item->surface(), &WSurface::subsurfaceOrderChanged, this, &CaptureSurfaceObserver::changed);
        connect(item, &WSurfaceItem::subsurfaceAdded, this, &CaptureSurfaceObserver::observeSubsurface);
        connect(item, &WSurfaceItem::subsurfaceRemoved, this, &CaptureSurfaceObserver::changed);
        connect(item, &WSurfaceItem::xChanged, this, &CaptureSurfaceObserver::changed);
        connect(item, &WSurfaceItem::yChanged, this, &CaptureSurfaceObserver::changed);
        connect(item, &WSurfaceItem::visibleChanged, this, &CaptureSurfaceObserver::changed);
        const auto children = item->findChildren<WSurfaceItem *>(QString(), Qt::FindDirectChildrenOnly);
        for (auto *child : children)
            new CaptureSurfaceObserver(child, content);
    }

private:
    void observeSubsurface(WSurfaceItem *item)
    {
        new CaptureSurfaceObserver(item, m_content);
        changed();
    }

    void committed(quint32 state)
    {
        constexpr quint32 visualState = WLR_SURFACE_STATE_BUFFER | WLR_SURFACE_STATE_SURFACE_DAMAGE
            | WLR_SURFACE_STATE_BUFFER_DAMAGE | WLR_SURFACE_STATE_TRANSFORM | WLR_SURFACE_STATE_SCALE
            | WLR_SURFACE_STATE_VIEWPORT | WLR_SURFACE_STATE_OFFSET;
        if (state & visualState)
            changed();
    }

    void changed() { Q_EMIT m_content->contentChanged(); }
    ToplevelCaptureContent *const m_content;
};

// A second view of the committed wl_surface tree, not a SurfaceWrapper proxy:
// those can configure/activate the shared shell surface. Capture is read-only.
class CaptureSurfaceItem final : public WSurfaceItem
{
public:
    CaptureSurfaceItem(SurfaceWrapper *wrapper, QQuickItem *parent)
        : WSurfaceItem(parent)
        , m_wrapper(wrapper)
    {
        setResizeMode(ManualResize);
        setFlags(RejectEvent);
    }

protected:
    bool doResizeSurface(const QSize &) override { return false; }

    QRectF getContentGeometry() const override
    {
        return shellSurface()->getContentGeometry();
    }

    QSizeF getContentSize() const override
    {
        return m_wrapper->type() == SurfaceWrapper::Type::XWayland
            ? getContentGeometry().size() : WSurfaceItem::getContentSize();
    }

private:
    SurfaceWrapper *const m_wrapper;
};
}

ToplevelCaptureContent::ToplevelCaptureContent(SurfaceWrapper *wrapper, WOutputRenderWindow *window)
    : QQuickItem(window->contentItem())
    , m_wrapper(wrapper)
    , m_surfaceItem(new CaptureSurfaceItem(wrapper, this))
    , m_scaleOutput(wrapper->ownsOutput() ? wrapper->ownsOutput()->output() : nullptr)
    , m_captureDpr(m_scaleOutput ? m_scaleOutput->scale() : window->effectiveDevicePixelRatio())
{
    auto *engine = WaylandServer::instance()->qmlEngine();
    QQmlEngine::setContextForObject(this, engine->rootContext());
    QQmlEngine::setContextForObject(m_surfaceItem, engine->rootContext());
    m_surfaceItem->setDelegate(engine->captureSurfaceContentComponent());
    m_surfaceItem->setShellSurface(wrapper->shellSurface());

    new CaptureSurfaceObserver(m_surfaceItem, this);
    // Observe unsynced changes even when no output is supplying frames.
    connect(window->renderControl(), &QQuickRenderControl::sceneChanged,
            this, &ToplevelCaptureContent::checkContentChanges, Qt::QueuedConnection);
    connect(window, &QQuickWindow::beforeSynchronizing,
            this, &ToplevelCaptureContent::checkContentChanges);

    auto *source = wrapper->surfaceItem();
    connect(source, &WSurfaceItem::widthChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::heightChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::surfaceSizeRatioChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::topPaddingChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::bottomPaddingChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::leftPaddingChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::rightPaddingChanged,
            this, &ToplevelCaptureContent::updateSurfaceGeometry);
    connect(source, &WSurfaceItem::flagsChanged,
            this, &ToplevelCaptureContent::updateSurfaceFlags);
    connect(source, &WSurfaceItem::smoothChanged, m_surfaceItem, &WSurfaceItem::setSmooth);
    connect(m_surfaceItem, &WSurfaceItem::boundingRectChanged,
            this, &ToplevelCaptureContent::updateGeometry);
    connect(wrapper, &SurfaceWrapper::noTitleBarChanged,
            this, &ToplevelCaptureContent::updateDecoration);
    connect(wrapper, &SurfaceWrapper::noDecorationChanged,
            this, &ToplevelCaptureContent::updateDecoration);
    connect(wrapper, &SurfaceWrapper::visibleDecorationChanged,
            this, &ToplevelCaptureContent::updateDecoration);
    connect(wrapper, &SurfaceWrapper::borderChanged,
            this, &ToplevelCaptureContent::updateDecoration);
    connect(wrapper, &SurfaceWrapper::radiusChanged,
            this, &ToplevelCaptureContent::updateDecoration);
    if (m_scaleOutput) {
        connect(m_scaleOutput, &WOutput::scaleChanged,
                this, &ToplevelCaptureContent::updateGeometry);
    }

    updateSurfaceFlags();
    m_surfaceItem->setSmooth(source->smooth());
    updateSurfaceGeometry();
}

SurfaceWrapper *ToplevelCaptureContent::sourceSurface() const
{
    return m_wrapper;
}

QRectF ToplevelCaptureContent::sourceRect() const
{
    return QRectF(QPointF(m_pixelBounds.topLeft()) / m_captureDpr, QSizeF(pixelSize()) / m_captureDpr);
}

ToplevelCaptureContent::~ToplevelCaptureContent()
{
    for (auto *source : std::as_const(m_textureSources))
        source->disconnect(this);
}

bool ToplevelCaptureContent::hasDirtyContent(QQuickItem *item)
{
    auto *d = QQuickItemPrivate::get(item);
    quint32 dirty = d->dirtyAttributes;
    if (auto *source = qobject_cast<QQuickShaderEffectSource *>(item)) {
        watchTextureSource(source);
        // A layer texture refresh marks Content even when its source is unchanged.
        if (!source->recursive())
            dirty &= ~QQuickItemPrivate::Content;
    }
    bool changed = dirty != 0;
    for (auto *child : std::as_const(d->childItems))
        changed |= hasDirtyContent(child);
    return changed;
}

void ToplevelCaptureContent::watchTextureSource(QQuickItem *item)
{
    auto *source = static_cast<QQuickShaderEffectSource *>(item);
    if (m_textureSources.contains(source))
        return;
    m_textureSources.insert(source);
    connect(source, &QObject::destroyed, this, [this](QObject *object) { m_textureSources.remove(object); });
    connect(source, &QQuickShaderEffectSource::sourceItemChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::sourceRectChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::textureSizeChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::formatChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::wrapModeChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::mipmapChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::textureMirroringChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::samplesChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::recursiveChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::hideSourceChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::liveChanged, this, &ToplevelCaptureContent::contentChanged);
    connect(source, &QQuickShaderEffectSource::scheduledUpdateCompleted,
            this, &ToplevelCaptureContent::textureUpdated);
}

void ToplevelCaptureContent::textureUpdated()
{
    auto *source = qobject_cast<QQuickShaderEffectSource *>(sender());
    if (source && !source->live())
        Q_EMIT contentChanged();
}

void ToplevelCaptureContent::checkContentChanges()
{
    if (hasDirtyContent(m_surfaceItem))
        Q_EMIT contentChanged();
}

void ToplevelCaptureContent::updateSurfaceFlags()
{
    m_surfaceItem->setFlags(m_wrapper->surfaceItem()->flags() | WSurfaceItem::RejectEvent);
}

void ToplevelCaptureContent::updateSurfaceGeometry()
{
    auto *source = m_wrapper->surfaceItem();
    m_surfaceItem->setSurfaceSizeRatio(source->surfaceSizeRatio());
    m_surfaceItem->setTopPadding(source->topPadding());
    m_surfaceItem->setBottomPadding(source->bottomPadding());
    m_surfaceItem->setLeftPadding(source->leftPadding());
    m_surfaceItem->setRightPadding(source->rightPadding());
    m_surfaceItem->setSize(source->size());
    updateDecoration();
}

void ToplevelCaptureContent::updateDecoration()
{
    auto *engine = WaylandServer::instance()->qmlEngine();
    if (m_wrapper->noTitleBar()) {
        if (m_titleBar) {
            m_titleBar->setVisible(false);
            m_titleBar->deleteLater();
            m_titleBar = nullptr;
        }
    } else if (!m_titleBar) {
        m_titleBar = engine->createTitleBar(m_wrapper, m_surfaceItem);
        m_titleBar->setZ(static_cast<int>(WSurfaceItem::ZOrder::ContentItem));
    }

    const bool borderVisible = !m_wrapper->noDecoration() && m_wrapper->visibleDecoration()
        && m_wrapper->borderVisible();
    if (borderVisible && !m_border) {
        m_border = engine->createBorder(m_surfaceItem);
        m_border->setZ(static_cast<int>(WSurfaceItem::ZOrder::ContentItem) + 1);
    }
    if (m_border) {
        m_border->setVisible(borderVisible);
        m_border->setSize(m_surfaceItem->size());
        m_border->setProperty("radius", m_wrapper->radius());
        m_border->setProperty("outsideColor", m_wrapper->borderColor());
        m_border->setProperty("borderWidth", m_wrapper->borderWidth());
    }
    updateGeometry();
}

void ToplevelCaptureContent::updateGeometry()
{
    const qreal dpr = m_scaleOutput ? m_scaleOutput->scale() : m_captureDpr;
    QRectF bounds = m_surfaceItem->boundingRect();
    if (!m_wrapper->noDecoration() && m_wrapper->visibleDecoration()
        && m_wrapper->borderVisible()) {
        // Only the outer SSD stroke extends beyond the surface tree. A shadow
        // is never created, and all bounds are local to the private input.
        const qreal border = m_wrapper->borderWidth();
        bounds |= QRectF(QPointF(), m_surfaceItem->size()).adjusted(-border, -border, border, border);
    }

    const QRect pixels(QPoint(qFloor(bounds.left() * dpr), qFloor(bounds.top() * dpr)),
                       QPoint(qCeil(bounds.right() * dpr) - 1, qCeil(bounds.bottom() * dpr) - 1));
    if (m_pixelBounds == pixels && m_captureDpr == dpr)
        return;
    m_pixelBounds = pixels;
    m_captureDpr = dpr;
    Q_EMIT captureGeometryChanged(sourceRect(), pixelSize(), captureDpr());
}
