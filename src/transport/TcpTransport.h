#pragma once

#include "transport/ITransport.h"

#include <QTcpSocket>

// RTU over TCP：把 Modbus RTU 帧原样塞进 TCP 流。
// 本地没有虚拟串口对（com0com）时，用这个通道和模拟下位机联调；
// 现场换成 SerialTransport 即可，上层代码一行不用改。
class TcpTransport : public ITransport
{
    Q_OBJECT
public:
    TcpTransport(const QString& host, quint16 port, QObject* parent = nullptr);

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool write(const QByteArray& data) override;
    QString describe() const override;

private:
    QTcpSocket m_socket;
    QString m_host;
    quint16 m_port;
};
