#include "transport/TransportChannel.h"

#include "core/PollEngine.h"
#include "transport/IoWorker.h"

TransportChannel::TransportChannel(QObject* parent)
    : QObject(parent)
{
}

TransportChannel* TransportChannel::createTcp(const QString& host, quint16 port, QObject* parent)
{
    auto* worker = new IoWorker();
    worker->configureTcp(host, port);
    auto* channel = new TransportChannel(parent);
    channel->startWorker(worker, QStringLiteral("TCP %1:%2 (RTU over TCP)").arg(host).arg(port));
    return channel;
}

TransportChannel* TransportChannel::createSerial(const QString& portName, int baud, QObject* parent)
{
    auto* worker = new IoWorker();
    worker->configureSerial(portName, baud);
    auto* channel = new TransportChannel(parent);
    channel->startWorker(worker, QStringLiteral("串口 %1 @ %2 8N1").arg(portName).arg(baud));
    return channel;
}

TransportChannel::~TransportChannel()
{
    if (m_worker) {
        if (m_engine) {
            // 引擎属于 I/O 线程：在它自己的线程里销毁，避免定时器跨线程析构
            QMetaObject::invokeMethod(m_worker, [this]() { delete m_engine; m_engine = nullptr; },
                                      Qt::BlockingQueuedConnection);
        }
        QMetaObject::invokeMethod(m_worker, "close", Qt::BlockingQueuedConnection);
        m_worker->deleteLater();
    }
    m_thread.quit();
    m_thread.wait(3000);
}

void TransportChannel::startWorker(IoWorker* worker, const QString& description)
{
    m_worker = worker;
    m_description = description;
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &IoWorker::bytesReceived, this, &TransportChannel::bytesReceived);
    connect(m_worker, &IoWorker::errorOccurred, this, &TransportChannel::errorOccurred);
    connect(m_worker, &IoWorker::closedByPeer, this, [this]() {
        m_connected = false;
        emit closed();
    });
    connect(m_worker, &IoWorker::connected, this, [this]() {
        m_connected = true;
        m_ioThreadId = m_worker->ioThreadId();
    });
    connect(m_worker, &IoWorker::opened, this, [this]() {
        m_ioThreadId = m_worker->ioThreadId();
    });

    m_thread.setObjectName(QStringLiteral("io-worker"));
    m_thread.start();
}

bool TransportChannel::open()
{
    if (!m_worker) {
        return false;
    }
    QMetaObject::invokeMethod(m_worker, "open", Qt::QueuedConnection);
    return true;
}

bool TransportChannel::isOpen() const
{
    return m_worker && m_worker->isConnected();   // 真实连接状态，跨线程安全
}

PollEngine* TransportChannel::createPollEngine()
{
    if (!m_worker) {
        return nullptr;
    }
    PollEngine* engine = nullptr;
    // 直接在 I/O 线程里 new，省掉 moveToThread：定时器、解析、收发同处一线程
    QMetaObject::invokeMethod(m_worker, [this, &engine]() { engine = new PollEngine(this); },
                              Qt::BlockingQueuedConnection);
    m_engine = engine;
    return engine;
}

void TransportChannel::close()
{
    m_connected = false;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "close", Qt::QueuedConnection);
    }
}

bool TransportChannel::write(const QByteArray& data)
{
    if (!m_worker || !m_worker->isConnected()) {
        emit errorOccurred(QStringLiteral("通道未连接，写入被丢弃"));
        return false;
    }
    if (QThread::currentThread() == &m_thread) {   // 采集引擎就在 I/O 线程里：直接写，不多绕一次队列
        m_worker->writeData(data);
    } else {
        QMetaObject::invokeMethod(m_worker, "writeData", Qt::QueuedConnection, Q_ARG(QByteArray, data));
    }
    return true;
}
