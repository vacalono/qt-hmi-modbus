#pragma once

#include "protocol/ModbusFrame.h"

#include <QObject>
#include <QElapsedTimer>
#include <QThread>
#include <QQueue>
#include <QTimer>
#include <QVector>
#include <atomic>
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

    // 这三个可以在任意线程调：若调用者不是引擎所在线程，会自动投递过去
public slots:
    void start();
    void stop();
    bool enqueueWrite(uint16_t addr, uint16_t value);

public:

    State state() const { return m_state; }
    QString stateText() const;
    quint64 okFrames() const   { return m_okFrames.load(std::memory_order_relaxed); }
    quint64 timeouts() const   { return m_timeouts.load(std::memory_order_relaxed); }
    quint64 crcErrors() const  { return m_crcErrors.load(std::memory_order_relaxed); }
    quint64 retries() const    { return m_retryCount.load(std::memory_order_relaxed); }
    double lastPollLatencyMs() const { return m_lastLatencyMs; }
    quint64 ticks() const { return m_ticks.load(std::memory_order_relaxed); }

signals:
    void sample(const QVector<uint16_t>& regs, qint64 timestampMs);
    void stateChanged(PollEngine::State state, const QString& detail);
    void frameLogged(const QString& dir, const QByteArray& frame, const QString& note);
    void writeFinished(bool ok, uint16_t addr, uint16_t value, const QString& detail);

private slots:
    void onTick();
    void onConnected();
    void onConnectWatch();
    void onBytes(const QByteArray& data);
    void onTimeout();
    void onTransportError(const QString& message);
    void onTransportClosed();

private:
    void initTimers();
    bool inOwnThread() const { return QThread::currentThread() == thread(); }
    void enterState(State s, const QString& detail);
    void sendNext();
    void handleReadResponse(const QByteArray& frame);
    void handleWriteResponse(const QByteArray& frame);
    void failAttempt(const QString& why);
    void scheduleReconnect();

    TransportChannel* m_transport = nullptr;
    // 定时器在 start() 时于引擎所在线程创建（QTimer 必须属于运行它的线程）
    QTimer* m_tick = nullptr;
    QTimer* m_deadline = nullptr;      // 单次请求的应答超时
    QTimer* m_reconnect = nullptr;     // 断线后的退避重连
    QTimer* m_connectWatch = nullptr;  // 等待 transport 真正连上（最多 3 s）
    QElapsedTimer m_connectClock;
    QElapsedTimer m_requestClock;

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

    int m_backoffMs = 500;
    int m_failStreak = 0;

    std::atomic<quint64> m_okFrames{0};
    std::atomic<quint64> m_timeouts{0};
    std::atomic<quint64> m_crcErrors{0};
    std::atomic<quint64> m_retryCount{0};
    std::atomic<quint64> m_ticks{0};
    double m_lastLatencyMs = 0.0;
};
