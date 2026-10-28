#include "transport/SerialTransport.h"

#include <QSerialPortInfo>

SerialTransport::SerialTransport(QObject* parent)
    : ITransport(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, [this]() {
        emit bytesReceived(m_port.readAll());
    });
    connect(&m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
        if (e == QSerialPort::NoError) {
            return;
        }
        emit errorOccurred(m_port.errorString());
    });
}

void SerialTransport::setPortName(const QString& name)
{
    m_portName = name;
}

void SerialTransport::setBaudRate(int baud)
{
    m_baud = baud;
}

bool SerialTransport::open()
{
    if (m_port.isOpen()) {
        return true;
    }
    m_port.setPortName(m_portName);
    m_port.setBaudRate(m_baud);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port.open(QIODevice::ReadWrite)) {
        emit errorOccurred(QStringLiteral("打开串口 %1 失败：%2").arg(m_portName, m_port.errorString()));
        return false;
    }
    return true;
}

void SerialTransport::close()
{
    if (m_port.isOpen()) {
        m_port.close();
        emit closed();
    }
}

bool SerialTransport::isOpen() const
{
    return m_port.isOpen();
}

bool SerialTransport::write(const QByteArray& data)
{
    if (!m_port.isOpen()) {
        emit errorOccurred(QStringLiteral("串口未打开，写入被丢弃"));
        return false;
    }
    const qint64 n = m_port.write(data);
    if (n != data.size()) {
        emit errorOccurred(QStringLiteral("串口写入不完整"));
        return false;
    }
    emit bytesSent(data);
    return true;
}

QString SerialTransport::describe() const
{
    return QStringLiteral("串口 %1 @ %2 8N1").arg(m_portName).arg(m_baud);
}

QStringList SerialTransport::availablePorts()
{
    QStringList names;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        names << info.portName();
    }
    return names;
}
