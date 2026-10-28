#pragma once

#include <QObject>
#include <QString>
#include <QThread>

class IoWorker;

// UI 侧通道：对外暴露与 ITransport 同样的一组方法/信号（open/close/isOpen/write/describe），
// 内部把调用投递到 I/O 线程里的 IoWorker，避免 UI 线程直接碰 socket/串口。
class TransportChannel : public QObject
{
    Q_OBJECT
public:
    static TransportChannel* createTcp(const QString& host, quint16 port, QObject* parent = nullptr);
    static TransportChannel* createSerial(const QString& portName, int baud, QObject* parent = nullptr);
    ~TransportChannel() override;

    bool open();
    void close();
    bool isOpen() const { return m_open; }
    bool write(const QByteArray& data);
    QString describe() const { return m_description; }
    quintptr ioThreadId() const { return m_ioThreadId; }

signals:
    void bytesReceived(const QByteArray& data);
    void errorOccurred(const QString& message);
    void closed();

private:
    explicit TransportChannel(QObject* parent = nullptr);
    void startWorker(IoWorker* worker, const QString& description);

    QThread m_thread;
    IoWorker* m_worker = nullptr;
    bool m_open = false;
    QString m_description;
    quintptr m_ioThreadId = 0;
};
