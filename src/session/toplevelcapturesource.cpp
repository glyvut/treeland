// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "toplevelcapturesource.h"
#include "toplevelcapturecontent.h"

#include "surface/surfacewrapper.h"
#include <woutputrenderwindow.h>

ToplevelCaptureSource::ToplevelCaptureSource(SurfaceWrapper *wrapper)
    : WExtImageCaptureSourceV1Impl(wrapper)
    , m_wrapper(wrapper)
{
}

QQuickItem *ToplevelCaptureSource::createCaptureContent()
{
    if (!m_wrapper || !m_wrapper->surfaceItem())
        return nullptr;
    auto *window = qobject_cast<WOutputRenderWindow *>(m_wrapper->surfaceItem()->window());
    if (!window)
        return nullptr;

    auto *content = new ToplevelCaptureContent(m_wrapper, window);
    connect(content, &ToplevelCaptureContent::captureGeometryChanged,
            this, &ToplevelCaptureSource::setCaptureGeometry);
    connect(content, &ToplevelCaptureContent::contentChanged,
            this, &ToplevelCaptureSource::handleContentChanged);
    setCaptureGeometry(content->sourceRect(), content->pixelSize(), content->captureDpr());
    return content;
}
