// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#include "core/rootsurfacecontainer.h"
#include "core/waylandserver.h"
#include "server-bridge.h"
#include "surface/surfacecontainer.h"
#include "surface/surfacewrapper.h"
#include "treeland-prelaunch-splash-desktop-v2.h"
#include "treelandconfig.hpp"
#include "workspace/workspace.h"

#include <QEventLoop>
#include <QPointer>
#include <QTimer>

#include <wbackend.h>
#include <woutputrenderwindow.h>

namespace {
constexpr auto SplashAppId = "org.deepin.treeland.protocol.splash";
constexpr auto IconSplashAppId = "org.deepin.treeland.protocol.splash-icon";
// Bounds of the state poll below, counted in rendered frames.
constexpr int MaxConvertFrames = 400;

QPointer<SurfaceWrapper> g_wrapper;
prelaunch_splash_desktop_state g_state {};
bool g_splashConfigReady = false;

bool isTestSplash(const SurfaceWrapper *wrapper)
{
    return wrapper && wrapper->type() == SurfaceWrapper::Type::SplashScreen
        && (wrapper->appId() == QLatin1String(SplashAppId)
            || wrapper->appId() == QLatin1String(IconSplashAppId));
}

void enablePrelaunchSplash(WaylandServer *server)
{
    server->globalConfig()->setEnablePrelaunchSplash(true);
    g_splashConfigReady = true;
}

void settleOneFrame()
{
    QEventLoop loop;
    QTimer::singleShot(5, &loop, &QEventLoop::quit);
    loop.exec();
}

void rememberSplash(SurfaceWrapper *wrapper)
{
    g_state = {};
    g_wrapper = wrapper;
    g_state.wrapper_created = 1;
    g_state.wrapper_in_workspace =
        WaylandServer::instance()->workspace()->surfaces().contains(wrapper) ? 1 : 0;
    g_state.wrapper_is_splash = 1;
    g_state.wrapper_has_qml_item = wrapper->prelaunchSplash() ? 1 : 0;
    g_state.wrapper_width = qRound(wrapper->implicitWidth());
    g_state.wrapper_height = qRound(wrapper->implicitHeight());
    qstrncpy(g_state.app_id, qPrintable(wrapper->appId()), sizeof(g_state.app_id));
    QObject::connect(wrapper, &QObject::destroyed, WaylandServer::instance(), [] {
        g_state.wrapper_destroyed = 1;
    });
}
}

void protocol_test_setup(WaylandServer *server)
{
    // A short real animation keeps the production geometry lifecycle.
    server->setAnimationSpeed(0.1f);
    add_headless_output(server->backend(), false);

    auto *config = server->globalConfig();
    if (config->isInitializeSucceeded()) {
        enablePrelaunchSplash(server);
    } else {
        QObject::connect(config,
                         &TreelandConfig::configInitializeSucceed,
                         server,
                         [server](auto *) { enablePrelaunchSplash(server); });
    }

    QObject::connect(server->workspace(), &SurfaceContainer::surfaceAdded, server, [](auto *wrapper) {
        if (isTestSplash(wrapper))
            rememberSplash(wrapper);
    });
}

extern "C" bool protocol_test_ready(WaylandServer *server)
{
    return g_splashConfigReady && !server->rootSurfaceContainer()->outputs().isEmpty();
}

extern "C" void prelaunch_splash_desktop_wait_for_creation(void *data)
{
    auto *created = static_cast<int *>(data);
    *created = g_wrapper ? 1 : 0;
    if (*created)
        return;

    QEventLoop eventLoop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
    guard.start(10000);
    const auto connection = QObject::connect(
        WaylandServer::instance()->workspace(),
        &SurfaceContainer::surfaceAdded,
        &eventLoop,
        [&eventLoop, created](SurfaceWrapper *wrapper) {
            if (isTestSplash(wrapper)) {
                *created = 1;
                eventLoop.quit();
            }
        });
    eventLoop.exec();
    QObject::disconnect(connection);
}

extern "C" void prelaunch_splash_desktop_wait_for_destruction(void *data)
{
    auto *destroyed = static_cast<int *>(data);
    *destroyed = g_state.wrapper_destroyed;
    if (*destroyed || !g_wrapper)
        return;

    QEventLoop eventLoop;
    QObject::connect(g_wrapper, &QObject::destroyed, &eventLoop, [&eventLoop, destroyed] {
        *destroyed = 1;
        eventLoop.quit();
    });
    eventLoop.exec();
}

extern "C" void prelaunch_splash_desktop_wait_converted(void *data)
{
    auto *ready = static_cast<int *>(data);
    for (int frame = 0; frame < MaxConvertFrames; ++frame) {
        WaylandServer::instance()->window()->render();
        if (g_wrapper && g_wrapper->type() == SurfaceWrapper::Type::XdgToplevel
            && !g_wrapper->prelaunchSplash()) {
            *ready = 1;
            return;
        }
        settleOneFrame();
    }
    qInfo() << "splash conversion not observed: wrapper" << g_wrapper.data() << "type"
            << (g_wrapper ? int(g_wrapper->type()) : -1) << "splashItem"
            << (g_wrapper ? g_wrapper->prelaunchSplash() : nullptr) << "surfaceItem"
            << (g_wrapper ? g_wrapper->surfaceItem() : nullptr) << "animating"
            << (g_wrapper ? g_wrapper->isAnimationRunning() : false);
}

extern "C" void prelaunch_splash_desktop_read_state(void *data)
{
    auto state = g_state;
    auto *server = WaylandServer::instance();
    state.output_ready = !server->rootSurfaceContainer()->outputs().isEmpty() ? 1 : 0;
    if (g_wrapper) {
        state.wrapper_in_workspace = server->workspace()->surfaces().contains(g_wrapper) ? 1 : 0;
        state.wrapper_is_splash = g_wrapper->type() == SurfaceWrapper::Type::SplashScreen ? 1 : 0;
        state.wrapper_has_qml_item = g_wrapper->prelaunchSplash() ? 1 : 0;
        state.wrapper_width = qRound(g_wrapper->implicitWidth());
        state.wrapper_height = qRound(g_wrapper->implicitHeight());
        state.converted_to_xdg =
            g_wrapper->type() == SurfaceWrapper::Type::XdgToplevel ? 1 : 0;
        state.splash_item_gone = g_wrapper->prelaunchSplash() ? 0 : 1;
        auto *icon = g_wrapper->appIcon().value<wlr_buffer *>();
        state.icon_ready = state.icon_width = state.icon_height = 0;
        if (icon) {
            state.icon_ready = 1;
            state.icon_width = icon->width;
            state.icon_height = icon->height;
        }
        qstrncpy(state.app_id, qPrintable(g_wrapper->appId()), sizeof(state.app_id));
    } else if (state.wrapper_created) {
        state.wrapper_in_workspace = 0;
    }
    *static_cast<prelaunch_splash_desktop_state *>(data) = state;
}
