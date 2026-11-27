// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <woutput.h>
#include <wsurfaceitem.h>

#include <QSet>

class SurfaceWrapper;
Q_MOC_INCLUDE("surface/surfacewrapper.h")

WAYLIB_SERVER_USE_NAMESPACE

class ToplevelCaptureContent final : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(SurfaceWrapper *sourceSurface READ sourceSurface CONSTANT)

public:
    ToplevelCaptureContent(SurfaceWrapper *wrapper, WOutputRenderWindow *window);
    ~ToplevelCaptureContent() override;
    SurfaceWrapper *sourceSurface() const;
    QSize pixelSize() const { return m_pixelBounds.size(); }
    QRectF sourceRect() const;
    qreal captureDpr() const { return m_captureDpr; }

Q_SIGNALS:
    void contentChanged();
    void captureGeometryChanged(const QRectF &sourceRect, const QSize &pixelSize, qreal dpr);

private:
    void updateSurfaceGeometry();
    void updateSurfaceFlags();
    void updateDecoration();
    void updateGeometry();
    void checkContentChanges();
    bool hasDirtyContent(QQuickItem *item);
    void watchTextureSource(QQuickItem *item);
    void textureUpdated();

    SurfaceWrapper *const m_wrapper;
    WSurfaceItem *const m_surfaceItem;
    const QPointer<WOutput> m_scaleOutput;
    QRect m_pixelBounds;
    qreal m_captureDpr;
    QPointer<QQuickItem> m_titleBar;
    QQuickItem *m_border = nullptr;
    QSet<QObject *> m_textureSources;
};
