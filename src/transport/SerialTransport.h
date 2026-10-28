#pragma once

#include "transport/ITransport.h"

#include <QSerialPort>

// 串口传输：现场对接 PLC / 板卡 / 仪表的真实通道，8N1，默认 115200。
// 本机没有物理串口和虚拟串口驱动，所以演示走 TcpTransport；两条通道共用同一套帧编解码。
class SerialTransport : public ITransport
{
    Q_OBJECT
public:
    explicit SerialTransport(QObject* parent = nullptr);

    void setPortName(const QString& name);
    void setBaudRate(int baud);

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool write(const QByteArray& data) override;
    QString describe() const override;

    static QStringList availablePorts();

private:
    QSerialPort m_port;
    QString m_portName;
    int m_baud = 115200;
};
