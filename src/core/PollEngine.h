#pragma once

#include "protocol/ModbusFrame.h"

#include <QObject>
#include <QElapsedTimer>
#include <QQueue>
#include <QTimer>
#include <QVector>
#include <cstdint>

class TransportChannel;

// 轮询采集引擎：定时发 0x03 读一批保持寄存器，做超时判定、重试、断线退避重连，
// 并把写寄存器的请求串行化到同一个请求槽里（同一时刻只允许一个在途请求，避免应答错配）。
class PollEngine : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Connecting, Online, Backoff, Stopped };
    Q_ENUM(State)

    explicit PollEngine(TransportChannel* transport, QObject* parent = nullptr);

    void setSlave(uint8_t slave)            { m_slave = slave; }
    void setRegisterBase(uint16_t addr)     { m_regBase = addr; }
    void setRegisterCount(uint16_t count)   { m_regCount = count; }
    void setIntervalMs(int ms)              { m_intervalMs = ms; }
    void setResponseTimeoutMs(int ms)       { m_timeoutMs = ms; }

    void start();
    void stop();
    bool enqueueWrite(uint16_t addr, uint16_t value);

    State state() const { return m_state; }
    QString stateText() const;
    quint64 okFrames() const   { return m_okFrames; }
    quint64 timeouts() const   { return m_timeouts; }
    quint64 crcErrors() const  { return m_crcErrors; }
    quint64 retries() const    { return m_retryCount; }

signals:
    void sample(const QVector<uint16_t>& regs, qint64 timestampMs);
    void stateChanged(PollEngine::State state, const QString& detail);
    void frameLogged(const QString& dir, const QByteArray& frame, const QString& note);
    void writeFinished(bool ok, uint16_t addr, uint16_t value, const QString& detail);

private slots:
    void onTick();
    void onConnectWatch();
    void onBytes(const QByteArray& data);
    void onTimeout();
    void onTransportError(const QString& message);
    void onTransportClosed();

private:
    void enterState(State s, const QString& detail);
    void sendNext();
    void handleReadResponse(const QByteArray& frame);
    void handleWriteResponse(const QByteArray& frame);
    void failAttempt(const QString& why);
    void scheduleReconnect();

    TransportChannel* m_transport = nullptr;
    QTimer m_tick;
    QTimer m_deadline;      // 单次请求的应答超时
    QTimer m_reconnect;     // 断线后的退避重连
    QTimer m_connectWatch;  // 等待 transport 真正连上（异步连接）
    QElapsedTimer m_connectClock;

    uint8_t  m_slave = 1;
    uint16_t m_regBase = 0;
    uint16_t m_regCount = 5;
    int      m_intervalMs = 300;
    int      m_timeoutMs = 250;

    State   m_state = State::Idle;
    QByteArray m_rx;

    bool     m_pendingWrite = false;
    bool     m_inFlight = false;    // 同一时刻只允许一个在途请求（一问一答）
    uint16_t m_pendingAddr = 0;
    uint16_t m_pendingValue = 0;
    QQueue<QPair<uint16_t, uint16_t>> m_writeQueue;

    int m_retryCount = 0;
    int m_backoffMs = 500;
    int m_failStreak = 0;

    quint64 m_okFrames = 0;
    quint64 m_timeouts = 0;
    quint64 m_crcErrors = 0;
};
