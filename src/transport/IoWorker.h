#pragma once

#include <QObject>
#include <QString>
#include <memory>

class ITransport;

// 把传输（QSerialPort / QTcpSocket）放进独立线程的对象。
// 创建在 UI 线程、`moveToThread` 后再 `open()`；所有方法都是槽，跨线程走队列连接。
// 这样 UI 线程忙（重绘、导出、用户拖动）时，I/O 的读写与字节接收不会被拖住。
class IoWorker : public QObject
{
    Q_OBJECT
public:
    explicit IoWorker(QObject* parent = nullptr);
    ~IoWorker() override;

    // 必须在 moveToThread 之前调用（只写配置，不碰设备）
    void configureTcp(const QString& host, quint16 port);
    void configureSerial(const QString& portName, int baud);

    quintptr ioThreadId() const { return m_ioThreadId; }

public slots:
    void open();
    void close();
    void writeData(const QByteArray& data);

signals:
    void opened();
    void closedByPeer();
    void bytesReceived(const QByteArray& data);
    void errorOccurred(const QString& message);

private:
    void setupTransport();

    std::unique_ptr<ITransport> m_transport;
    bool m_serial = false;
    QString m_host;
    quint16 m_port = 15020;
    QString m_portName;
    int m_baud = 115200;
    quintptr m_ioThreadId = 0;
};
