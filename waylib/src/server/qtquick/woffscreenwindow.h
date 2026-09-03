// Copyright (C) 2024-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wglobal.h>
#include <woutput.h>
#include <wtextureproviderprovider.h>

#include <QPointer>
#include <QQuickItem>

WAYLIB_SERVER_BEGIN_NAMESPACE

class WBufferRenderer;
class WSGTextureProvider;
class WQuickTextureProxy;
class WAYLIB_SERVER_EXPORT WOffscreenWindow : public QQuickItem, public virtual WTextureProviderProvider
{
    Q_OBJECT
    Q_PROPERTY(QQuickItem* source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    Q_PROPERTY(WOutput* output READ output WRITE setOutput NOTIFY outputChanged REQUIRED)
    Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio WRITE setDevicePixelRatio NOTIFY devicePixelRatioChanged)

public:
    explicit WOffscreenWindow(QQuickItem *parent = nullptr);
    ~WOffscreenWindow();

    Q_INVOKABLE void invalidate();

    bool isTextureProvider() const override;
    QSGTextureProvider *textureProvider() const override;
    WSGTextureProvider *wTextureProvider() const override;
    WOutputRenderWindow *outputRenderWindow() const override;

    QQuickItem *source() const;
    void setSource(QQuickItem *source);

    WOutput *output() const;
    void setOutput(WOutput *output);

    qreal devicePixelRatio() const;
    void setDevicePixelRatio(qreal devicePixelRatio);

    QSize pixelSize() const;
    wlr_buffer *buffer() const;

    // Called by WOutputRenderWindow during a render pass.
    void renderContent();

Q_SIGNALS:
    void sourceChanged();
    void outputChanged();
    void devicePixelRatioChanged();

private:
    void ensureRenderer();
    void ensureProxy();
    void releaseProxy();
    void itemChange(ItemChange, const ItemChangeData &) override;

    WBufferRenderer *m_renderer = nullptr;
    WQuickTextureProxy *m_proxy = nullptr;
    QPointer<QQuickItem> m_source = nullptr;
    WOutput *m_output = nullptr;
    qreal m_devicePixelRatio = 1.0;
};

WAYLIB_SERVER_END_NAMESPACE
