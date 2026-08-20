// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qwayland-xdg-activation-v1.h"
#include "qwayland-treeland-window-animation-v1.h"

#include <private/qwaylandwindow_p.h>

#include <QApplication>
#include <QDebug>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <QtWaylandClient/QWaylandClientExtension>

#include <QList>
#include <utility>

#include <cstdlib>

// ---------------------------------------------------------------------------
// Protocol client wrappers — managers (globals)
// ---------------------------------------------------------------------------

class XdgActivationManager
    : public QWaylandClientExtensionTemplate<XdgActivationManager>
    , public QtWayland::xdg_activation_v1
{
    Q_OBJECT
public:
    explicit XdgActivationManager()
        : QWaylandClientExtensionTemplate<XdgActivationManager>(1)
    {
    }
    ~XdgActivationManager() override
    {
        if (isInitialized())
            destroy();
    }
};

class WindowAnimationManager
    : public QWaylandClientExtensionTemplate<WindowAnimationManager>
    , public QtWayland::treeland_window_animation_manager_v1
{
    Q_OBJECT
public:
    explicit WindowAnimationManager()
        : QWaylandClientExtensionTemplate<WindowAnimationManager>(1)
    {
    }
    ~WindowAnimationManager() override
    {
        if (isInitialized())
            destroy();
    }
};

// ---------------------------------------------------------------------------
// Per-object wrappers
// ---------------------------------------------------------------------------

class ActivationToken : public QObject, public QtWayland::xdg_activation_token_v1
{
    Q_OBJECT
public:
    explicit ActivationToken(::xdg_activation_token_v1 *obj, QObject *parent = nullptr)
        : QObject(parent)
        , QtWayland::xdg_activation_token_v1(obj)
    {
    }

    QString tokenString() const { return m_token; }

protected:
    void xdg_activation_token_v1_done(const QString &token) override
    {
        m_token = token;
        Q_EMIT done(token);
    }

Q_SIGNALS:
    void done(const QString &token);

private:
    QString m_token;
};

class WindowAnimationRect : public QObject, public QtWayland::treeland_window_animation_rect_v1
{
    Q_OBJECT
public:
    explicit WindowAnimationRect(::treeland_window_animation_rect_v1 *obj, QObject *parent = nullptr)
        : QObject(parent)
        , QtWayland::treeland_window_animation_rect_v1(obj)
    {
    }

protected:
    void treeland_window_animation_rect_v1_closed() override
    {
        // The target window this rect is for has been destroyed, so the rect
        // is no longer needed. Let the owner destroy it so rects for already
        // closed windows do not accumulate.
        Q_EMIT closed();
    }

Q_SIGNALS:
    void closed();
};

// ---------------------------------------------------------------------------
// Test image
// ---------------------------------------------------------------------------

static QPixmap createTestImage(int w, int h)
{
    QPixmap pm(w, h);
    QPainter p(&pm);
    QLinearGradient grad(0, 0, w, h);
    grad.setColorAt(0, QColor(63, 81, 181));
    grad.setColorAt(1, QColor(33, 150, 243));
    p.fillRect(0, 0, w, h, grad);
    p.setPen(QPen(QColor(255, 255, 255, 200), 4));
    p.drawRoundedRect(10, 10, w - 20, h - 20, 20, 20);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPointSize(28);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, "Window\nAnimation");
    return pm;
}

// ---------------------------------------------------------------------------
// Image widget
// ---------------------------------------------------------------------------

class ImageWidget : public QWidget
{
public:
    explicit ImageWidget(const QPixmap &pixmap, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_pixmap(pixmap)
    {
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, m_pixmap.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    }

private:
    QPixmap m_pixmap;
};

// ---------------------------------------------------------------------------
// Helper: get wl_surface from a widget
// ---------------------------------------------------------------------------

static ::wl_surface *getWlSurface(QWidget *widget)
{
    QWindow *handle = widget->windowHandle();
    if (!handle || !handle->handle())
        return nullptr;
    auto *waylandWindow = static_cast<QtWaylandClient::QWaylandWindow *>(handle->handle());
    return waylandWindow ? waylandWindow->surface() : nullptr;
}

// ---------------------------------------------------------------------------
// Sender (Application A)
// ---------------------------------------------------------------------------

class SenderWindow : public QMainWindow
{
    Q_OBJECT
public:
    SenderWindow(XdgActivationManager *actMgr, WindowAnimationManager *animMgr)
        : m_actMgr(actMgr)
        , m_animMgr(animMgr)
        , m_image(createTestImage(200, 200))
    {
        setWindowTitle("Window Animation - Sender (press Space)");

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);

        m_imageWidget = new ImageWidget(m_image, central);
        m_imageWidget->setFixedSize(200, 200);
        layout->addWidget(m_imageWidget, 0, Qt::AlignCenter);

        auto *label = new QLabel("Press SPACE to launch receiver\n"
                                 "The rect stays alive for close animation.", central);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);

        setCentralWidget(central);
        resize(400, 300);
    }

    ~SenderWindow() override
    {
        // Destroy all rect objects so the compositor falls back
        // to the default close animation once sender is gone.
        for (auto *rect : std::as_const(m_rects)) {
            rect->destroy();
        }
        m_rects.clear();
    }
    
protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Space)
            launchReceiver();
        QMainWindow::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        // The image widget is centered in the layout, so resizing the main
        // window moves it. Re-commit the persistent rect(s) with the updated
        // geometry so the open/close animation follows the new position.
        updateRectGeometries();
        QMainWindow::resizeEvent(event);
    }

private:

    // Geometry (relative to this sender surface) of the image widget that is
    // the source of the window animation.
    QRect animationRect() const
    {
        const QPoint imagePos = m_imageWidget->mapTo(this, QPoint(0, 0));
        return QRect(imagePos, m_imageWidget->size());
    }

    void updateRectGeometries()
    {
        if (m_rects.isEmpty())
            return;
        const QRect rect = animationRect();
        for (auto *r : std::as_const(m_rects)) {
            r->set_geometry(rect.x(), rect.y(), rect.width(), rect.height());
            r->commit();
        }
    }

    void launchReceiver()
    {
        if (!m_actMgr || !m_actMgr->isInitialized()) {
            qWarning() << "xdg_activation_v1 not available";
            return;
        }

        ::wl_surface *surface = getWlSurface(this);
        if (!surface) {
            qWarning() << "Cannot get wl_surface";
            return;
        }

        // 1. Create activation token
        auto *rawToken = m_actMgr->get_activation_token();
        if (!rawToken) {
            qWarning() << "Failed to create activation token";
            return;
        }
        auto *token = new ActivationToken(rawToken, this);

        connect(token, &ActivationToken::done, this, [this](const QString &tokenStr) {
            qInfo() << "Got activation token, launching receiver...";

            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("XDG_ACTIVATION_TOKEN", tokenStr);

            QProcess *proc = new QProcess(this);
            proc->setProcessEnvironment(env);
            proc->start(QCoreApplication::applicationFilePath(), {"--receiver"});
        });

        // 2. Set the originating surface on the token
        token->set_surface(surface);

        // 3. Attach window animation rect (relative to this surface).
        //    The rect is persistent: it stays alive after commit and is used
        //    for both the open and close animations. We keep a reference so
        //    the compositor can read the latest geometry when B's window
        //    closes. Destroying the rect (or disconnecting) makes the
        //    compositor fall back to the default close animation.
        if (m_animMgr && m_animMgr->isInitialized()) {
            const QRect rectGeo = animationRect();
            auto *rawRect = m_animMgr->get_window_animation_rect(rawToken);
            if (rawRect) {
                // Each launched receiver gets its own persistent rect. The
                // rect stays alive (associated with that receiver's window)
                // so it can drive that window's close animation. We must NOT
                // destroy a previous rect here: doing so clears the close
                // animation of a still-open earlier receiver.
                auto *rect = new WindowAnimationRect(rawRect, this);
                // When the compositor reports that the rect's target window
                // has closed, the rect is useless: destroy it and drop it
                // from our list.
                connect(rect, &WindowAnimationRect::closed, this, [this, rect] {
                    const int idx = m_rects.indexOf(rect);
                    if (idx >= 0)
                        m_rects.removeAt(idx);
                    rect->destroy();
                });
                rect->set_geometry(rectGeo.x(), rectGeo.y(), rectGeo.width(), rectGeo.height());
                rect->commit();
                m_rects.append(rect);
            }
        }

        // 4. Commit the token to receive the token string
        token->commit();
    }

    XdgActivationManager *m_actMgr;
    WindowAnimationManager *m_animMgr;
    QList<WindowAnimationRect *> m_rects;
    QPixmap m_image;
    QWidget *m_imageWidget = nullptr;
};

// ---------------------------------------------------------------------------
// Receiver (Application B)
// ---------------------------------------------------------------------------

class ReceiverWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ReceiverWindow(XdgActivationManager *actMgr)
        : m_actMgr(actMgr)
        , m_image(createTestImage(800, 600))
    {
        setWindowTitle("Window Animation - Receiver");
        resize(800, 600);

        auto *central = new ImageWidget(m_image, this);
        setCentralWidget(central);
    }

    void activateWithToken()
    {
        QByteArray tokenEnv = qgetenv("XDG_ACTIVATION_TOKEN");
        if (tokenEnv.isEmpty()) {
            qWarning() << "No XDG_ACTIVATION_TOKEN set";
            return;
        }
        qputenv("XDG_ACTIVATION_TOKEN", "");

        if (!m_actMgr || !m_actMgr->isInitialized()) {
            qWarning() << "xdg_activation_v1 not available";
            return;
        }

        ::wl_surface *surface = getWlSurface(this);
        if (!surface) {
            qWarning() << "Cannot get wl_surface for receiver";
            return;
        }

        qInfo() << "Activating with token...";
        m_actMgr->activate(QString::fromUtf8(tokenEnv), surface);
    }

private:
    XdgActivationManager *m_actMgr;
    QPixmap m_image;
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    bool receiverMode = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--receiver") == 0)
            receiverMode = true;
    }

    XdgActivationManager actMgr;
    WindowAnimationManager animMgr;

    if (receiverMode) {
        ReceiverWindow window(&actMgr);
        window.show();
        window.activateWithToken();
        return app.exec();
    } else {
        SenderWindow window(&actMgr, &animMgr);
        window.show();
        return app.exec();
    }
}

#include "main.moc"
