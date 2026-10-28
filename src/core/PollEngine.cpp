#include "core/PollEngine.h"
#include "transport/IoWorker.h"
#include "transport/TransportChannel.h"

#include <QDateTime>

PollEngine::PollEngine(TransportChannel* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
    // 引擎跑在 I/O 线程里（与 IoWorker 同线程）：字节到达是直连，不经过 UI 线程
    if (IoWorker* worker = m_transport->ioWorker()) {
        connect(worker, &IoWorker::bytesReceived, this, &PollEngine::onBytes);
        connect(worker, &IoWorker::errorOccurred, this, &PollEngine::onTransportError);
        connect(worker, &IoWorker::closedByPeer, this, &PollEngine::onTransportClosed);
        connect(worker, &IoWorker::connected, this, &PollEngine::onConnected);
    }
}

void PollEngine::initTimers()
{
    if (m_tick) {
        return;   // 已在引擎线程建好
    }
    m_tick = new QTimer(this);
    m_tick->setTimerType(Qt::PreciseTimer);
    connect(m_tick, &QTimer::timeout, this, &PollEngine::onTick);

    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, &PollEngine::onTimeout);

    m_reconnect = new QTimer(this);
    m_reconnect->setSingleShot(true);
    connect(m_reconnect, &QTimer::timeout, this, [this]() {
        enterState(State::Connecting, QStringLiteral("退避结束，重连中"));
        m_connectClock.start();
        m_connectWatch->start(3000);
        if (!m_transport->open()) {
            scheduleReconnect();
        }
    });

    m_connectWatch = new QTimer(this);
    m_connectWatch->setSingleShot(true);
    connect(m_connectWatch, &QTimer::timeout, this, &PollEngine::onConnectWatch);
}

QString PollEngine::stateText() const
{
    switch (m_state) {
    case State::Idle:       return QStringLiteral("未连接");
    case State::Connecting: return QStringLiteral("连接中");
    case State::Online:     return QStringLiteral("在线");
    case State::Backoff:    return QStringLiteral("断线重连中");
    case State::Stopped:    return QStringLiteral("已停止");
    }
    return QStringLiteral("未知");
}

void PollEngine::start()
{
    if (!inOwnThread()) {   // 从 UI 线程调用：投递到引擎线程再执行
        QMetaObject::invokeMethod(this, [this]() { start(); }, Qt::QueuedConnection);
        return;
    }
    initTimers();
    m_rx.clear();
    m_writeQueue.clear();
    m_pendingWrite = false;
    m_inFlight = false;
    m_failStreak = 0;
    m_backoffMs = 500;
    enterState(State::Connecting, QStringLiteral("开始连接 %1").arg(m_transport->describe()));
    m_connectClock.start();
    if (!m_transport->open()) {
        scheduleReconnect();
        return;
    }
    m_connectWatch->start(3000);   // 串口会立刻回 connected，TCP 要等
}

void PollEngine::onConnected()
{
    if (m_state != State::Connecting && m_state != State::Backoff) {
        return;
    }
    m_connectWatch->stop();
    enterState(State::Online, QStringLiteral("已连接 %1").arg(m_transport->describe()));
    m_tick->start(m_intervalMs);
    m_failStreak = 0;
}

void PollEngine::onConnectWatch()
{
    if (m_state != State::Connecting) {
        return;
    }
    enterState(State::Backoff, QStringLiteral("连接超时（3 s）"));
    scheduleReconnect();
}

void PollEngine::stop()
{
    if (!inOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
        return;
    }
    if (m_tick) {
        m_tick->stop();
        m_deadline->stop();
        m_reconnect->stop();
        m_connectWatch->stop();
    }
    m_transport->close();
    enterState(State::Stopped, QStringLiteral("已停止采集"));
}

bool PollEngine::enqueueWrite(uint16_t addr, uint16_t value)
{
    if (!inOwnThread()) {
        QMetaObject::invokeMethod(this, [this, addr, value]() { enqueueWrite(addr, value); },
                                  Qt::QueuedConnection);
        return true;   // 入队是乐观的：真正的失败由 writeFinished 回来
    }
    if (m_state != State::Online) {
        return false;
    }
    m_writeQueue.enqueue(qMakePair(addr, value));
    return true;
}

void PollEngine::onTick()
{
    ++m_ticks;
    if (m_state != State::Online || m_inFlight) {
        return;   // 有在途请求时不再发新请求，保证一问一答
    }
    sendNext();
}

void PollEngine::sendNext()
{
    m_rx.clear();
    m_inFlight = true;
    m_requestClock.start();
    if (!m_writeQueue.isEmpty()) {
        const auto job = m_writeQueue.dequeue();
        m_pendingWrite = true;
        m_pendingAddr = job.first;
        m_pendingValue = job.second;
        const QByteArray frame = mb::buildWriteSingle(m_slave, job.first, job.second);
        emit frameLogged(QStringLiteral("TX"), frame,
                         QStringLiteral("写寄存器 %1 = %2").arg(job.first).arg(job.second));
        m_transport->write(frame);
        m_deadline->start(m_timeoutMs);
        return;
    }
    m_pendingWrite = false;
    const QByteArray frame = mb::buildReadHolding(m_slave, m_regBase, m_regCount);
    emit frameLogged(QStringLiteral("TX"), frame,
                     QStringLiteral("读寄存器 %1..%2").arg(m_regBase).arg(m_regBase + m_regCount - 1));
    m_transport->write(frame);
    m_deadline->start(m_timeoutMs);
}

void PollEngine::onBytes(const QByteArray& data)
{
    m_rx.append(data);
    const int expected = m_pendingWrite ? 8 : mb::expectedReadResponseSize(m_regCount);
    if (m_rx.size() < expected) {
        return;   // 半包：等后续字节
    }
    const QByteArray frame = m_rx.left(expected);
    m_rx.remove(0, expected);
    m_deadline->stop();
    m_lastLatencyMs = static_cast<double>(m_requestClock.elapsed());
    if (m_pendingWrite) {
        handleWriteResponse(frame);
    } else {
        handleReadResponse(frame);
    }
}

void PollEngine::handleReadResponse(const QByteArray& frame)
{
    m_inFlight = false;
    const mb::Response r = mb::parseReadHolding(frame, m_slave, m_regCount);
    if (!r.ok) {
        if (r.error.contains(QStringLiteral("CRC"))) {
            ++m_crcErrors;
        }
        emit frameLogged(QStringLiteral("RX"), frame, QStringLiteral("解析失败：%1").arg(r.error));
        failAttempt(r.error);
        return;
    }
    ++m_okFrames;
    m_failStreak = 0;
    m_backoffMs = 500;
    emit frameLogged(QStringLiteral("RX"), frame, QStringLiteral("OK"));
    emit sample(r.registers, QDateTime::currentMSecsSinceEpoch());
}

void PollEngine::handleWriteResponse(const QByteArray& frame)
{
    m_inFlight = false;
    const mb::Response r = mb::parseWriteSingle(frame, m_slave, m_pendingAddr, m_pendingValue);
    m_pendingWrite = false;
    emit frameLogged(QStringLiteral("RX"), frame, r.ok ? QStringLiteral("写入回显一致") : r.error);
    emit writeFinished(r.ok, m_pendingAddr, m_pendingValue, r.error);
    if (!r.ok) {
        failAttempt(r.error);
    }
}

void PollEngine::onTimeout()
{
    ++m_timeouts;
    m_inFlight = false;
    m_pendingWrite = false;
    m_rx.clear();          // 迟到的应答不能算到下一次请求上
    failAttempt(QStringLiteral("应答超时（%1 ms）").arg(m_timeoutMs));
}

void PollEngine::failAttempt(const QString& why)
{
    ++m_failStreak;
    if (m_failStreak < 3) {
        ++m_retryCount;
        return;   // 连续 3 次失败才判定断线，避免偶发丢帧就重连
    }
    enterState(State::Backoff, QStringLiteral("连续 3 次失败：%1").arg(why));
    m_tick->stop();
    m_transport->close();
    scheduleReconnect();
}

void PollEngine::scheduleReconnect()
{
    m_reconnect->start(m_backoffMs);
    m_backoffMs = qMin(m_backoffMs * 2, 8000);   // 0.5s → 1s → 2s → 4s → 8s 封顶
}

void PollEngine::onTransportError(const QString& message)
{
    if (m_state == State::Stopped) {
        return;
    }
    emit frameLogged(QStringLiteral("ERR"), QByteArray(), message);
    if (m_state == State::Connecting || m_state == State::Online) {
        ++m_failStreak;
        if (m_failStreak >= 3) {
            enterState(State::Backoff, message);
            if (m_tick) {
                m_tick->stop();
            }
            scheduleReconnect();
        }
    }
}

void PollEngine::onTransportClosed()
{
    if (m_state == State::Stopped || m_state == State::Backoff) {
        return;
    }
    enterState(State::Backoff, QStringLiteral("连接被对端关闭"));
    if (m_tick) {
        m_tick->stop();
    }
    scheduleReconnect();
}

void PollEngine::enterState(State s, const QString& detail)
{
    m_state = s;
    emit stateChanged(s, detail);
}
