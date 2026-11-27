// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wextimagecapturesourcev1impl.h>

#include <QPointer>

WAYLIB_SERVER_USE_NAMESPACE

class SurfaceWrapper;

class ToplevelCaptureSource final : public WExtImageCaptureSourceV1Impl
{
public:
    explicit ToplevelCaptureSource(SurfaceWrapper *wrapper);

protected:
    QQuickItem *createCaptureContent() override;

private:
    QPointer<SurfaceWrapper> m_wrapper;
};
