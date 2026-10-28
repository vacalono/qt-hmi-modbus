#include "transport/IoWorker.h"

#include "transport/SerialTransport.h"
#include "transport/TcpTransport.h"

#include <QThread>

IoWorker::IoWorker(QObject* parent)
    : QObject(parent)
{
    // 注意：QTimer 必须属于运行它的线程。本对象会被 moveToThread，
    // 所以定时器在 open()（I/O 线程内）里再建，不能在构造函数里建。
}

IoWorker::~IoWorker()
{
    if (m_transport) {
        m_transport->close();
    }
}

void IoWorker::configureTcp(const QString& host, quint16 port)
{
    m_serial = false;
    m_host = host;
    m_port = port;
}

void IoWorker::configureSerial(const QString& portName, int baud)
{
    m_serial = true;
    m_portName = portName;
    m_baud = baud;
}

void IoWorker::setupTransport()
{
    if (m_transport) {
        return;
    }
    if (m_serial) {
        auto* serial = new SerialTransport();
        serial->setPortName(m_portName);
        serial->setBaudRate(m_baud);
        m_transport.reset(serial);
    } else {
        m_transport.reset(new TcpTransport(m_host, m_port));
    }
    // 传输层的信号在本线程内直连，再由本对象转发给 UI 线程（自动走队列）
    connect(m_transport.get(), &ITransport::bytesReceived, this, &IoWorker::bytesReceived);
    connect(m_transport.get(), &ITransport::errorOccurred, this, &IoWorker::errorOccurred);
    connect(m_transport.get(), &ITransport::closed, this, [this]() {
        if (m_watch) {
            m_watch->stop();
        }
        m_connected = false;
        emit closedByPeer();
    });
    connect(m_transport.get(), &ITransport::bytesSent, this, [](const QByteArray&) {});
}

void IoWorker::open()
{
    m_ioThreadId = reinterpret_cast<quintptr>(QThread::currentThread());
    setupTransport();
    m_connected = false;
    if (!m_transport->open()) {
        return;
    }
    emit opened();
    if (!m_watch) {                        // 第一次 open()：在 I/O 线程里建轮询定时器
        m_watch = new QTimer(this);
        m_watch->setInterval(50);
        connect(m_watch, &QTimer::timeout, this, [this]() {
            if (!m_transport) {
                m_watch->stop();
                return;
            }
            if (m_transport->isOpen()) {
                m_watch->stop();
                if (!m_connected) {
                    m_connected = true;
                    emit connected();
                }
            }
        });
    }
    if (m_transport->isOpen()) {          // 串口：open 同步成功
        m_connected = true;
        emit connected();
        return;
    }
    m_watch->start();                      // TCP：等连上，最多 3 s（超时由引擎判）
}

void IoWorker::close()
{
    if (m_watch) {
        m_watch->stop();
    }
    m_connected = false;
    if (m_transport) {
        m_transport->close();
    }
}

void IoWorker::writeData(const QByteArray& data)
{
    if (m_transport) {
        m_transport->write(data);
    }
}
