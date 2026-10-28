#include "transport/IoWorker.h"

#include "transport/SerialTransport.h"
#include "transport/TcpTransport.h"

#include <QThread>

IoWorker::IoWorker(QObject* parent)
    : QObject(parent)
{
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
    connect(m_transport.get(), &ITransport::closed, this, &IoWorker::closedByPeer);
    connect(m_transport.get(), &ITransport::bytesSent, this, [](const QByteArray&) {});
}

void IoWorker::open()
{
    m_ioThreadId = reinterpret_cast<quintptr>(QThread::currentThread());
    setupTransport();
    if (!m_transport->open()) {
        return;
    }
    // TCP 是异步连接：这里不能假设立刻连上，交给上层按 isOpen + 超时判断
    emit opened();
}

void IoWorker::close()
{
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
