#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

// 传输层抽象：上层（PollEngine）只面对"写一帧、收一段字节"，不关心是串口还是 TCP。
// 这样做了两件事：
// 1) 同一套 Modbus 帧编解码能在串口（现场）和 RTU-over-TCP（本地联调）上复用；
// 2) 采集逻辑不依赖 QSerialPort，单元测试可以塞一个假传输进来。
class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool write(const QByteArray& data) = 0;
    virtual QString describe() const = 0;

signals:
    void bytesReceived(const QByteArray& data);
    void bytesSent(const QByteArray& data);
    void errorOccurred(const QString& message);
    void closed();
};
