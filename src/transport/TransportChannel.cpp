#include "transport/TransportChannel.h"

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
        m_open = false;
        emit closed();
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
    m_open = true;   // 逻辑连接：真实连接结果由 I/O 线程回传（TCP 异步）
    QMetaObject::invokeMethod(m_worker, "open", Qt::QueuedConnection);
    return true;
}

void TransportChannel::close()
{
    m_open = false;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "close", Qt::QueuedConnection);
    }
}

bool TransportChannel::write(const QByteArray& data)
{
    if (!m_worker || !m_open) {
        emit errorOccurred(QStringLiteral("通道未打开，写入被丢弃"));
        return false;
    }
    QMetaObject::invokeMethod(m_worker, "writeData", Qt::QueuedConnection, Q_ARG(QByteArray, data));
    return true;
}
