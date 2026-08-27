#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QTcpServer>
#include <QTcpSocket>
#include <QBuffer>
#include <QThread>
#include <QByteArray>
#include <QPainter>
#include <QMutex>
#include <QMutexLocker>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <atomic>
#include <cstring>

enum PacketType : quint8
{
    PACKET_FRAME = 1,
    PACKET_MOUSE = 2,
    PACKET_KEYBOARD = 3,
    PACKET_WHEEL = 4
};

#pragma pack(push, 1)

struct PacketHeader
{
    quint32 magic;
    quint8 type;
    quint32 size;
};

struct MousePacket
{
    quint16 x;
    quint16 y;
    quint8 button;
    quint8 pressed;
};

struct KeyboardPacket
{
    quint32 keysym;
    quint8 pressed;
};

struct WheelPacket
{
    qint16 delta;
};

#pragma pack(pop)

static constexpr quint32 PACKET_MAGIC = 0x56444B31;
static constexpr quint32 MAX_PACKET_SIZE = 20 * 1024 * 1024;
static constexpr int MAX_STREAM_WIDTH = 1280;


class ScreenCaptureWorker : public QObject
{
    Q_OBJECT

public:

    explicit ScreenCaptureWorker(QObject *parent = nullptr)
        : QObject(parent),
          display(nullptr),
          timer(nullptr),
          running(false),
          shmImage(nullptr),
          shmInfo{},
          shmAttached(false),
          screenWidth(0),
          screenHeight(0)
    {
    }

    ~ScreenCaptureWorker()
    {
        cleanup();
    }

public slots:

    void start()
    {
        if (running)
            return;

        display = XOpenDisplay(nullptr);

        if (!display)
        {
            emit error(
                "Cannot open X11 display"
            );
            return;
        }

        int screen =
            DefaultScreen(display);

        screenWidth =
            DisplayWidth(
                display,
                screen
            );

        screenHeight =
            DisplayHeight(
                display,
                screen
            );

        if (!setupXShm())
        {
            emit error(
                "XShm unavailable - capture cannot start"
            );

            cleanup();
            return;
        }

        running = true;

        timer =
            new QTimer(this);

        connect(
            timer,
            &QTimer::timeout,
            this,
            &ScreenCaptureWorker::captureFrame
        );

        /*
         * 20 FPS.
         *
         * Because frames are resized before JPEG
         * compression, this is much lighter than
         * compressing the native-resolution desktop.
         */
        timer->start(50);

        emit started(
            screenWidth,
            screenHeight
        );
    }

    void stop()
    {
        running = false;

        if (timer)
        {
            timer->stop();
            delete timer;
            timer = nullptr;
        }

        cleanup();
    }

signals:

    void frameReady(
        const QByteArray &data
    );

    void error(
        const QString &message
    );

    void started(
        int width,
        int height
    );

private:

    Display *display;
    QTimer *timer;

    std::atomic<bool> running;

    XImage *shmImage;
    XShmSegmentInfo shmInfo;
    bool shmAttached;

    int screenWidth;
    int screenHeight;

    bool setupXShm()
    {
        if (!display)
            return false;

        if (!XShmQueryExtension(display))
            return false;

        int screen =
            DefaultScreen(display);

        Window root =
            RootWindow(
                display,
                screen
            );

        shmImage =
            XShmCreateImage(
                display,
                DefaultVisual(
                    display,
                    screen
                ),
                DefaultDepth(
                    display,
                    screen
                ),
                ZPixmap,
                nullptr,
                &shmInfo,
                screenWidth,
                screenHeight
            );

        if (!shmImage)
            return false;

        shmInfo.shmid =
            shmget(
                IPC_PRIVATE,
                shmImage->bytes_per_line *
                    shmImage->height,
                IPC_CREAT | 0600
            );

        if (shmInfo.shmid < 0)
        {
            XDestroyImage(shmImage);
            shmImage = nullptr;
            return false;
        }

        shmInfo.shmaddr =
            static_cast<char *>(
                shmat(
                    shmInfo.shmid,
                    nullptr,
                    0
                )
            );

        if (shmInfo.shmaddr ==
            reinterpret_cast<char *>(-1))
        {
            shmctl(
                shmInfo.shmid,
                IPC_RMID,
                nullptr
            );

            XDestroyImage(shmImage);
            shmImage = nullptr;

            return false;
        }

        shmInfo.readOnly = False;

        shmImage->data =
            shmInfo.shmaddr;

        if (!XShmAttach(
                display,
                &shmInfo))
        {
            shmdt(
                shmInfo.shmaddr
            );

            shmctl(
                shmInfo.shmid,
                IPC_RMID,
                nullptr
            );

            XDestroyImage(shmImage);
            shmImage = nullptr;

            return false;
        }

        XSync(
            display,
            False
        );

        shmAttached = true;

        return true;
    }

    void captureFrame()
    {
        if (!running ||
            !display ||
            !shmImage)
        {
            return;
        }

        int screen =
            DefaultScreen(display);

        Window root =
            RootWindow(
                display,
                screen
            );

        if (!XShmGetImage(
                display,
                root,
                shmImage,
                0,
                0,
                AllPlanes))
        {
            return;
        }

        QImage source(
            reinterpret_cast<const uchar *>(
                shmImage->data
            ),
            shmImage->width,
            shmImage->height,
            shmImage->bytes_per_line,
            QImage::Format_RGB32
        );

        QImage frame =
            source.copy();

        /*
         * Do not send a 1920x1080/2560x1440/4K
         * JPEG when the viewer doesn't need it.
         *
         * Maximum stream width = 1280.
         */
        if (frame.width() >
            MAX_STREAM_WIDTH)
        {
            int newWidth =
                MAX_STREAM_WIDTH;

            int newHeight =
                static_cast<int>(
                    static_cast<double>(
                        frame.height()
                    ) *
                    newWidth /
                    frame.width()
                );

            frame =
                frame.scaled(
                    newWidth,
                    newHeight,
                    Qt::KeepAspectRatio,
                    Qt::FastTransformation
                );
        }

        QByteArray jpeg;

        QBuffer buffer(
            &jpeg
        );

        buffer.open(
            QIODevice::WriteOnly
        );

        frame.save(
            &buffer,
            "JPEG",
            40
        );

        if (!jpeg.isEmpty())
        {
            emit frameReady(
                jpeg
            );
        }
    }

    void cleanup()
    {
        running = false;

        if (timer)
        {
            timer->stop();
            delete timer;
            timer = nullptr;
        }

        if (display &&
            shmAttached)
        {
            XShmDetach(
                display,
                &shmInfo
            );

            XSync(
                display,
                False
            );

            shmAttached = false;
        }

        if (shmInfo.shmaddr &&
            shmInfo.shmaddr !=
                reinterpret_cast<char *>(-1))
        {
            shmdt(
                shmInfo.shmaddr
            );

            shmInfo.shmaddr = nullptr;
        }

        if (shmInfo.shmid > 0)
        {
            shmctl(
                shmInfo.shmid,
                IPC_RMID,
                nullptr
            );

            shmInfo.shmid = -1;
        }

        if (shmImage)
        {
            XDestroyImage(
                shmImage
            );

            shmImage = nullptr;
        }

        if (display)
        {
            XCloseDisplay(
                display
            );

            display = nullptr;
        }
    }
};


class RemoteCanvas : public QWidget
{
    Q_OBJECT

public:

    explicit RemoteCanvas(
        QWidget *parent = nullptr
    )
        : QWidget(parent),
          socket(nullptr),
          remoteImage()
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);

        setStyleSheet(
            "background:#020617;"
            "border:1px solid #1e293b;"
        );
    }

    void setSocket(
        QTcpSocket *s
    )
    {
        socket = s;
    }

    void setRemoteImage(
        const QImage &image
    )
    {
        remoteImage = image;

        update();
    }

protected:

    void paintEvent(
        QPaintEvent *
    ) override
    {
        QPainter painter(this);

        painter.fillRect(
            rect(),
            QColor(
                2,
                6,
                23
            )
        );

        if (remoteImage.isNull())
        {
            painter.setPen(
                Qt::white
            );

            painter.drawText(
                rect(),
                Qt::AlignCenter,
                "Waiting for remote screen..."
            );

            return;
        }

        QImage scaled =
            remoteImage.scaled(
                size(),
                Qt::KeepAspectRatio,
                Qt::FastTransformation
            );

        int x =
            (width() -
             scaled.width()) /
            2;

        int y =
            (height() -
             scaled.height()) /
            2;

        painter.drawImage(
            x,
            y,
            scaled
        );
    }

    void mouseMoveEvent(
        QMouseEvent *event
    ) override
    {
        sendMouse(
            event->position().toPoint(),
            0,
            false
        );

        event->accept();
    }

    void mousePressEvent(
        QMouseEvent *event
    ) override
    {
        int button = 1;

        if (event->button() ==
            Qt::RightButton)
        {
            button = 3;
        }
        else if (
            event->button() ==
            Qt::MiddleButton)
        {
            button = 2;
        }

        sendMouse(
            event->position().toPoint(),
            button,
            true
        );

        setFocus(
            Qt::MouseFocusReason
        );

        event->accept();
    }

    void mouseReleaseEvent(
        QMouseEvent *event
    ) override
    {
        int button = 1;

        if (event->button() ==
            Qt::RightButton)
        {
            button = 3;
        }
        else if (
            event->button() ==
            Qt::MiddleButton)
        {
            button = 2;
        }

        sendMouse(
            event->position().toPoint(),
            button,
            false
        );

        event->accept();
    }

    void wheelEvent(
        QWheelEvent *event
    ) override
    {
        if (!socket ||
            socket->state() !=
                QAbstractSocket::ConnectedState)
        {
            return;
        }

        WheelPacket packet;

        packet.delta =
            static_cast<qint16>(
                event->angleDelta().y() /
                120
            );

        sendPacket(
            PACKET_WHEEL,
            reinterpret_cast<const char *>(
                &packet
            ),
            sizeof(packet)
        );

        event->accept();
    }

    void keyPressEvent(
        QKeyEvent *event
    ) override
    {
        if (event->isAutoRepeat())
            return;

        KeySym key =
            convertKey(
                event->key()
            );

        if (key == NoSymbol)
            return;

        KeyboardPacket packet;

        packet.keysym =
            static_cast<quint32>(
                key
            );

        packet.pressed = 1;

        sendPacket(
            PACKET_KEYBOARD,
            reinterpret_cast<const char *>(
                &packet
            ),
            sizeof(packet)
        );

        event->accept();
    }

    void keyReleaseEvent(
        QKeyEvent *event
    ) override
    {
        if (event->isAutoRepeat())
            return;

        KeySym key =
            convertKey(
                event->key()
            );

        if (key == NoSymbol)
            return;

        KeyboardPacket packet;

        packet.keysym =
            static_cast<quint32>(
                key
            );

        packet.pressed = 0;

        sendPacket(
            PACKET_KEYBOARD,
            reinterpret_cast<const char *>(
                &packet
            ),
            sizeof(packet)
        );

        event->accept();
    }

private:

    QTcpSocket *socket;

    QImage remoteImage;

    void sendPacket(
        quint8 type,
        const char *data,
        quint32 size
    )
    {
        if (!socket ||
            socket->state() !=
                QAbstractSocket::ConnectedState)
        {
            return;
        }

        PacketHeader header;

        header.magic =
            PACKET_MAGIC;

        header.type =
            type;

        header.size =
            size;

        socket->write(
            reinterpret_cast<const char *>(
                &header
            ),
            sizeof(header)
        );

        socket->write(
            data,
            size
        );
    }

    void sendMouse(
        QPoint point,
        int button,
        bool pressed
    )
    {
        if (!socket ||
            socket->state() !=
                QAbstractSocket::ConnectedState)
        {
            return;
        }

        /*
         * Calculate the actual image rectangle.
         * This prevents the mouse coordinates from
         * becoming incorrect because of letterboxing.
         */
        if (remoteImage.isNull())
            return;

        QSize scaledSize =
            remoteImage.size();

        scaledSize.scale(
            size(),
            Qt::KeepAspectRatio
        );

        int offsetX =
            (width() -
             scaledSize.width()) /
            2;

        int offsetY =
            (height() -
             scaledSize.height()) /
            2;

        int localX =
            point.x() -
            offsetX;

        int localY =
            point.y() -
            offsetY;

        localX =
            qBound(
                0,
                localX,
                scaledSize.width()
            );

        localY =
            qBound(
                0,
                localY,
                scaledSize.height()
            );

        double x =
            static_cast<double>(
                localX
            ) /
            static_cast<double>(
                scaledSize.width()
            );

        double y =
            static_cast<double>(
                localY
            ) /
            static_cast<double>(
                scaledSize.height()
            );

        MousePacket packet;

        packet.x =
            static_cast<quint16>(
                qBound(
                    0.0,
                    x,
                    1.0
                ) * 65535.0
            );

        packet.y =
            static_cast<quint16>(
                qBound(
                    0.0,
                    y,
                    1.0
                ) * 65535.0
            );

        packet.button =
            static_cast<quint8>(
                button
            );

        packet.pressed =
            pressed ? 1 : 0;

        sendPacket(
            PACKET_MOUSE,
            reinterpret_cast<const char *>(
                &packet
            ),
            sizeof(packet)
        );
    }

    KeySym convertKey(
        int key
    )
    {
        switch (key)
        {
        case Qt::Key_A: return XK_a;
        case Qt::Key_B: return XK_b;
        case Qt::Key_C: return XK_c;
        case Qt::Key_D: return XK_d;
        case Qt::Key_E: return XK_e;
        case Qt::Key_F: return XK_f;
        case Qt::Key_G: return XK_g;
        case Qt::Key_H: return XK_h;
        case Qt::Key_I: return XK_i;
        case Qt::Key_J: return XK_j;
        case Qt::Key_K: return XK_k;
        case Qt::Key_L: return XK_l;
        case Qt::Key_M: return XK_m;
        case Qt::Key_N: return XK_n;
        case Qt::Key_O: return XK_o;
        case Qt::Key_P: return XK_p;
        case Qt::Key_Q: return XK_q;
        case Qt::Key_R: return XK_r;
        case Qt::Key_S: return XK_s;
        case Qt::Key_T: return XK_t;
        case Qt::Key_U: return XK_u;
        case Qt::Key_V: return XK_v;
        case Qt::Key_W: return XK_w;
        case Qt::Key_X: return XK_x;
        case Qt::Key_Y: return XK_y;
        case Qt::Key_Z: return XK_z;

        case Qt::Key_0: return XK_0;
        case Qt::Key_1: return XK_1;
        case Qt::Key_2: return XK_2;
        case Qt::Key_3: return XK_3;
        case Qt::Key_4: return XK_4;
        case Qt::Key_5: return XK_5;
        case Qt::Key_6: return XK_6;
        case Qt::Key_7: return XK_7;
        case Qt::Key_8: return XK_8;
        case Qt::Key_9: return XK_9;

        case Qt::Key_Space: return XK_space;
        case Qt::Key_Return: return XK_Return;
        case Qt::Key_Enter: return XK_KP_Enter;
        case Qt::Key_Backspace: return XK_BackSpace;
        case Qt::Key_Tab: return XK_Tab;
        case Qt::Key_Escape: return XK_Escape;
        case Qt::Key_Delete: return XK_Delete;
        case Qt::Key_Insert: return XK_Insert;
        case Qt::Key_Home: return XK_Home;
        case Qt::Key_End: return XK_End;
        case Qt::Key_PageUp: return XK_Page_Up;
        case Qt::Key_PageDown: return XK_Page_Down;

        case Qt::Key_Left: return XK_Left;
        case Qt::Key_Right: return XK_Right;
        case Qt::Key_Up: return XK_Up;
        case Qt::Key_Down: return XK_Down;

        case Qt::Key_Shift: return XK_Shift_L;
        case Qt::Key_Control: return XK_Control_L;
        case Qt::Key_Alt: return XK_Alt_L;
        case Qt::Key_Meta: return XK_Super_L;

        case Qt::Key_CapsLock:
            return XK_Caps_Lock;

        case Qt::Key_NumLock:
            return XK_Num_Lock;

        case Qt::Key_F1: return XK_F1;
        case Qt::Key_F2: return XK_F2;
        case Qt::Key_F3: return XK_F3;
        case Qt::Key_F4: return XK_F4;
        case Qt::Key_F5: return XK_F5;
        case Qt::Key_F6: return XK_F6;
        case Qt::Key_F7: return XK_F7;
        case Qt::Key_F8: return XK_F8;
        case Qt::Key_F9: return XK_F9;
        case Qt::Key_F10: return XK_F10;
        case Qt::Key_F11: return XK_F11;
        case Qt::Key_F12: return XK_F12;

        case Qt::Key_Comma: return XK_comma;
        case Qt::Key_Period: return XK_period;
        case Qt::Key_Slash: return XK_slash;
        case Qt::Key_Backslash: return XK_backslash;
        case Qt::Key_Minus: return XK_minus;
        case Qt::Key_Equal: return XK_equal;
        case Qt::Key_Semicolon: return XK_semicolon;
        case Qt::Key_Apostrophe: return XK_apostrophe;
        case Qt::Key_BracketLeft: return XK_bracketleft;
        case Qt::Key_BracketRight: return XK_bracketright;
        case Qt::Key_QuoteLeft: return XK_grave;

        default:
            return NoSymbol;
        }
    }
};


class VesselDeskWindow :
    public QMainWindow
{
    Q_OBJECT

public:

    VesselDeskWindow()
        : server(nullptr),
          hostSocket(nullptr),
          clientSocket(nullptr),
          captureThread(nullptr),
          captureWorker(nullptr),
          stopping(false)
    {
        setWindowTitle(
            "VesselDesk"
        );

        resize(
            1200,
            800
        );

        QWidget *central =
            new QWidget(this);

        QVBoxLayout *layout =
            new QVBoxLayout(
                central
            );

        layout->setContentsMargins(
            10,
            10,
            10,
            10
        );

        QHBoxLayout *toolbar =
            new QHBoxLayout();

        QLabel *title =
            new QLabel(
                "VesselDesk",
                this
            );

        title->setStyleSheet(
            "font-size:20px;"
            "font-weight:bold;"
            "color:#38bdf8;"
        );

        ipEdit =
            new QLineEdit(
                this
            );

        ipEdit->setPlaceholderText(
            "Remote Tailscale IP"
        );

        connectButton =
            new QPushButton(
                "Connect Remote",
                this
            );

        hostButton =
            new QPushButton(
                "Start Host Mode",
                this
            );

        stopButton =
            new QPushButton(
                "Stop Host",
                this
            );

        toolbar->addWidget(title);
        toolbar->addWidget(ipEdit);
        toolbar->addWidget(connectButton);
        toolbar->addWidget(hostButton);
        toolbar->addWidget(stopButton);

        canvas =
            new RemoteCanvas(
                this
            );

        layout->addLayout(
            toolbar
        );

        layout->addWidget(
            canvas,
            1
        );

        setCentralWidget(
            central
        );

        connect(
            connectButton,
            &QPushButton::clicked,
            this,
            &VesselDeskWindow::connectRemote
        );

        connect(
            hostButton,
            &QPushButton::clicked,
            this,
            &VesselDeskWindow::startHost
        );

        connect(
            stopButton,
            &QPushButton::clicked,
            this,
            &VesselDeskWindow::stopHost
        );

        applyTheme();

        statusBar()->showMessage(
            "Ready"
        );
    }

    ~VesselDeskWindow()
    {
        stopping = true;

        stopCapture();

        if (hostSocket)
        {
            hostSocket->disconnectFromHost();
        }

        if (server)
        {
            server->close();
        }
    }

private slots:

    void startHost()
    {
        if (server)
        {
            statusBar()->showMessage(
                "Host already running"
            );

            return;
        }

        server =
            new QTcpServer(
                this
            );

        if (!server->listen(
                QHostAddress::Any,
                5900))
        {
            statusBar()->showMessage(
                "Failed to listen on TCP 5900"
            );

            server->deleteLater();
            server = nullptr;

            return;
        }

        connect(
            server,
            &QTcpServer::newConnection,
            this,
            &VesselDeskWindow::acceptHostConnection
        );

        /*
         * IMPORTANT:
         *
         * Hide the VesselDesk window first.
         * Capture is started only after a delay.
         *
         * This prevents the host window from appearing
         * in the captured desktop and causing recursive
         * screen images.
         */
        hide();

        statusBar()->showMessage(
            "Host active"
        );

        QTimer::singleShot(
            700,
            this,
            [this]()
            {
                if (!server)
                    return;

                statusBar()->showMessage(
                    "Host waiting for remote connection"
                );
            }
        );
    }

    void stopHost()
    {
        stopCapture();

        if (hostSocket)
        {
            hostSocket->disconnectFromHost();
            hostSocket->deleteLater();
            hostSocket = nullptr;
        }

        if (server)
        {
            server->close();
            server->deleteLater();
            server = nullptr;
        }

        show();

        activateWindow();

        raise();

        canvas->setRemoteImage(
            QImage()
        );

        statusBar()->showMessage(
            "Host stopped"
        );
    }

    void acceptHostConnection()
    {
        if (!server)
            return;

        /*
         * Only one remote client is supported.
         */
        if (hostSocket)
        {
            hostSocket->disconnectFromHost();
            hostSocket->deleteLater();
            hostSocket = nullptr;
        }

        hostSocket =
            server->nextPendingConnection();

        hostInputBuffer.clear();

        connect(
            hostSocket,
            &QTcpSocket::readyRead,
            this,
            &VesselDeskWindow::processHostInput
        );

        connect(
            hostSocket,
            &QTcpSocket::disconnected,
            this,
            [this]()
            {
                stopCapture();

                statusBar()->showMessage(
                    "Remote client disconnected"
                );
            }
        );

        statusBar()->showMessage(
            "Remote client connected"
        );

        startCapture();
    }

    void connectRemote()
    {
        QString ip =
            ipEdit->text().trimmed();

        if (ip.isEmpty())
        {
            statusBar()->showMessage(
                "Enter the remote Tailscale IP"
            );

            return;
        }

        if (clientSocket)
        {
            clientSocket->disconnectFromHost();
            clientSocket->deleteLater();
            clientSocket = nullptr;
        }

        clientSocket =
            new QTcpSocket(
                this
            );

        canvas->setSocket(
            clientSocket
        );

        clientInputBuffer.clear();

        connect(
            clientSocket,
            &QTcpSocket::connected,
            this,
            [this]()
            {
                statusBar()->showMessage(
                    "Connected to remote VesselDesk"
                );

                canvas->setFocus(
                    Qt::OtherFocusReason
                );
            }
        );

        connect(
            clientSocket,
            &QTcpSocket::readyRead,
            this,
            &VesselDeskWindow::processFrames
        );

        connect(
            clientSocket,
            &QTcpSocket::disconnected,
            this,
            [this]()
            {
                statusBar()->showMessage(
                    "Remote disconnected"
                );
            }
        );

        connect(
            clientSocket,
            &QTcpSocket::errorOccurred,
            this,
            [this](
                QAbstractSocket::SocketError
            )
            {
                statusBar()->showMessage(
                    clientSocket->errorString()
                );
            }
        );

        clientSocket->connectToHost(
            ip,
            5900
        );
    }

    void processHostInput()
    {
        if (!hostSocket)
            return;

        hostInputBuffer.append(
            hostSocket->readAll()
        );

        while (true)
        {
            if (hostInputBuffer.size() <
                static_cast<int>(
                    sizeof(PacketHeader)))
            {
                return;
            }

            PacketHeader header;

            memcpy(
                &header,
                hostInputBuffer.constData(),
                sizeof(PacketHeader)
            );

            if (header.magic !=
                PACKET_MAGIC)
            {
                hostInputBuffer.clear();
                return;
            }

            if (header.size >
                MAX_PACKET_SIZE)
            {
                hostInputBuffer.clear();
                return;
            }

            int total =
                sizeof(PacketHeader) +
                static_cast<int>(
                    header.size
                );

            if (hostInputBuffer.size() <
                total)
            {
                return;
            }

            QByteArray data =
                hostInputBuffer.mid(
                    sizeof(PacketHeader),
                    header.size
                );

            hostInputBuffer.remove(
                0,
                total
            );

            if (header.type ==
                PACKET_MOUSE &&
                data.size() ==
                sizeof(MousePacket))
            {
                MousePacket packet;

                memcpy(
                    &packet,
                    data.constData(),
                    sizeof(packet)
                );

                injectMouse(
                    packet
                );
            }
            else if (
                header.type ==
                PACKET_KEYBOARD &&
                data.size() ==
                sizeof(KeyboardPacket))
            {
                KeyboardPacket packet;

                memcpy(
                    &packet,
                    data.constData(),
                    sizeof(packet)
                );

                injectKeyboard(
                    packet
                );
            }
            else if (
                header.type ==
                PACKET_WHEEL &&
                data.size() ==
                sizeof(WheelPacket))
            {
                WheelPacket packet;

                memcpy(
                    &packet,
                    data.constData(),
                    sizeof(packet)
                );

                injectWheel(
                    packet
                );
            }
        }
    }

    void processFrames()
    {
        if (!clientSocket)
            return;

        clientInputBuffer.append(
            clientSocket->readAll()
        );

        /*
         * If an old backlog becomes enormous,
         * discard it and wait for a fresh frame.
         */
        if (clientInputBuffer.size() >
            8 * 1024 * 1024)
        {
            clientInputBuffer.clear();
            return;
        }

        while (true)
        {
            if (clientInputBuffer.size() <
                static_cast<int>(
                    sizeof(PacketHeader)))
            {
                return;
            }

            PacketHeader header;

            memcpy(
                &header,
                clientInputBuffer.constData(),
                sizeof(PacketHeader)
            );

            if (header.magic !=
                PACKET_MAGIC)
            {
                clientInputBuffer.clear();
                return;
            }

            if (header.size >
                MAX_PACKET_SIZE)
            {
                clientInputBuffer.clear();
                return;
            }

            int total =
                sizeof(PacketHeader) +
                static_cast<int>(
                    header.size
                );

            if (clientInputBuffer.size() <
                total)
            {
                return;
            }

            QByteArray data =
                clientInputBuffer.mid(
                    sizeof(PacketHeader),
                    header.size
                );

            clientInputBuffer.remove(
                0,
                total
            );

            if (header.type !=
                PACKET_FRAME)
            {
                continue;
            }

            QImage image;

            if (!image.loadFromData(
                    data,
                    "JPEG"))
            {
                continue;
            }

            canvas->setRemoteImage(
                image
            );
        }
    }

    void sendFrame(
        const QByteArray &jpeg
    )
    {
        if (!hostSocket ||
            hostSocket->state() !=
                QAbstractSocket::ConnectedState)
        {
            return;
        }

        /*
         * This is extremely important for latency.
         *
         * Never allow several old frames to queue.
         */
        if (hostSocket->bytesToWrite() >
            512 * 1024)
        {
            return;
        }

        PacketHeader header;

        header.magic =
            PACKET_MAGIC;

        header.type =
            PACKET_FRAME;

        header.size =
            static_cast<quint32>(
                jpeg.size()
            );

        hostSocket->write(
            reinterpret_cast<const char *>(
                &header
            ),
            sizeof(header)
        );

        hostSocket->write(
            jpeg
        );
    }

    void startCapture()
    {
        if (captureThread)
            return;

        captureThread =
            new QThread(
                this
            );

        captureWorker =
            new ScreenCaptureWorker();

        captureWorker->moveToThread(
            captureThread
        );

        connect(
            captureThread,
            &QThread::started,
            captureWorker,
            &ScreenCaptureWorker::start
        );

        connect(
            captureWorker,
            &ScreenCaptureWorker::frameReady,
            this,
            &VesselDeskWindow::sendFrame,
            Qt::QueuedConnection
        );

        connect(
            captureWorker,
            &ScreenCaptureWorker::error,
            this,
            [this](
                const QString &message
            )
            {
                statusBar()->showMessage(
                    message
                );
            }
        );

        connect(
            captureThread,
            &QThread::finished,
            captureWorker,
            &QObject::deleteLater
        );

        captureThread->start();
    }

    void stopCapture()
    {
        if (!captureThread)
            return;

        if (captureWorker)
        {
            QMetaObject::invokeMethod(
                captureWorker,
                "stop",
                Qt::BlockingQueuedConnection
            );
        }

        captureThread->quit();

        captureThread->wait();

        captureThread->deleteLater();

        captureThread = nullptr;
        captureWorker = nullptr;
    }

private:

    QLineEdit *ipEdit;

    QPushButton *connectButton;
    QPushButton *hostButton;
    QPushButton *stopButton;

    RemoteCanvas *canvas;

    QTcpServer *server;

    QTcpSocket *hostSocket;
    QTcpSocket *clientSocket;

    QThread *captureThread;

    ScreenCaptureWorker *captureWorker;

    QByteArray hostInputBuffer;
    QByteArray clientInputBuffer;

    bool stopping;

    void injectMouse(
        const MousePacket &packet
    )
    {
        static Display *display =
            nullptr;

        if (!display)
        {
            display =
                XOpenDisplay(
                    nullptr
                );
        }

        if (!display)
            return;

        int screen =
            DefaultScreen(
                display
            );

        int width =
            DisplayWidth(
                display,
                screen
            );

        int height =
            DisplayHeight(
                display,
                screen
            );

        int x =
            static_cast<int>(
                packet.x *
                width /
                65535.0
            );

        int y =
            static_cast<int>(
                packet.y *
                height /
                65535.0
            );

        XTestFakeMotionEvent(
            display,
            -1,
            x,
            y,
            CurrentTime
        );

        if (packet.button != 0)
        {
            XTestFakeButtonEvent(
                display,
                packet.button,
                packet.pressed
                    ? True
                    : False,
                CurrentTime
            );
        }

        XFlush(
            display
        );
    }

    void injectKeyboard(
        const KeyboardPacket &packet
    )
    {
        static Display *display =
            nullptr;

        if (!display)
        {
            display =
                XOpenDisplay(
                    nullptr
                );
        }

        if (!display)
            return;

        KeySym keysym =
            static_cast<KeySym>(
                packet.keysym
            );

        KeyCode keycode =
            XKeysymToKeycode(
                display,
                keysym
            );

        if (keycode == 0)
            return;

        XTestFakeKeyEvent(
            display,
            keycode,
            packet.pressed
                ? True
                : False,
            CurrentTime
        );

        XFlush(
            display
        );
    }

    void injectWheel(
        const WheelPacket &packet
    )
    {
        static Display *display =
            nullptr;

        if (!display)
        {
            display =
                XOpenDisplay(
                    nullptr
                );
        }

        if (!display)
            return;

        int button =
            packet.delta > 0
                ? 4
                : 5;

        int count =
            qAbs(
                packet.delta
            );

        for (int i = 0;
             i < count;
             ++i)
        {
            XTestFakeButtonEvent(
                display,
                button,
                True,
                CurrentTime
            );

            XTestFakeButtonEvent(
                display,
                button,
                False,
                CurrentTime
            );
        }

        XFlush(
            display
        );
    }

    void applyTheme()
    {
        qApp->setStyle(
            QStyleFactory::create(
                "Fusion"
            )
        );

        QPalette palette;

        palette.setColor(
            QPalette::Window,
            QColor(15,23,42)
        );

        palette.setColor(
            QPalette::WindowText,
            QColor(241,245,249)
        );

        palette.setColor(
            QPalette::Base,
            QColor(30,41,59)
        );

        palette.setColor(
            QPalette::Text,
            QColor(241,245,249)
        );

        palette.setColor(
            QPalette::Button,
            QColor(30,41,59)
        );

        palette.setColor(
            QPalette::ButtonText,
            QColor(241,245,249)
        );

        palette.setColor(
            QPalette::Highlight,
            QColor(14,165,233)
        );

        qApp->setPalette(
            palette
        );

        qApp->setStyleSheet(
            "QLineEdit{"
            "background:#1e293b;"
            "border:1px solid #334155;"
            "padding:8px;"
            "border-radius:6px;"
            "color:#f8fafc;"
            "}"

            "QPushButton{"
            "background:#0284c7;"
            "border:none;"
            "color:white;"
            "padding:9px 16px;"
            "border-radius:6px;"
            "font-weight:bold;"
            "}"

            "QPushButton:hover{"
            "background:#38bdf8;"
            "color:#020617;"
            "}"

            "QStatusBar{"
            "background:#0f172a;"
            "color:#94a3b8;"
            "}"
        );
    }
};


int main(
    int argc,
    char *argv[]
)
{
    /*
     * Required before using X11 from multiple threads.
     */
    XInitThreads();

    QApplication app(
        argc,
        argv
    );

    VesselDeskWindow window;

    window.show();

    return app.exec();
}

#include "main.moc"
