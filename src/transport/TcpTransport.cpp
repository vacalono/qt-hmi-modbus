#include "transport/TcpTransport.h"

TcpTransport::TcpTransport(const QString& host, quint16 port, QObject* parent)
    : ITransport(parent), m_host(host), m_port(port)
{
    connect(&m_socket, &QTcpSocket::readyRead, this, [this]() {
        emit bytesReceived(m_socket.readAll());
    });
    connect(&m_socket, &QTcpSocket::connected, this, [this]() {
        m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);   // 小帧别攒 Nagle
    });
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(m_socket.errorString());
    });
    connect(&m_socket, &QTcpSocket::disconnected, this, [this]() {
        emit closed();
    });
}

bool TcpTransport::open()
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        return true;
    }
    m_socket.connectToHost(m_host, m_port);   // 异步：结果由 connected / errorOccurred 通知
    return true;
}

void TcpTransport::close()
{
    m_socket.abort();
}

bool TcpTransport::isOpen() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

bool TcpTransport::write(const QByteArray& data)
{
    if (!isOpen()) {
        emit errorOccurred(QStringLiteral("TCP 未连接，写入被丢弃"));
        return false;
    }
    const qint64 n = m_socket.write(data);
    if (n != data.size()) {
        emit errorOccurred(QStringLiteral("TCP 写入不完整"));
        return false;
    }
    emit bytesSent(data);
    return true;
}

QString TcpTransport::describe() const
{
    return QStringLiteral("TCP %1:%2 (RTU over TCP)").arg(m_host).arg(m_port);
}
