#include "core/PollEngine.h"
#include "transport/TransportChannel.h"

#include <QDateTime>

PollEngine::PollEngine(TransportChannel* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
    m_tick.setTimerType(Qt::PreciseTimer);
    connect(&m_tick, &QTimer::timeout, this, &PollEngine::onTick);

    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, &PollEngine::onTimeout);

    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, [this]() {
        enterState(State::Connecting, QStringLiteral("退避结束，重连中"));
        if (!m_transport->open()) {
            scheduleReconnect();
        }
    });

    connect(&m_connectWatch, &QTimer::timeout, this, &PollEngine::onConnectWatch);

    connect(m_transport, &TransportChannel::bytesReceived, this, &PollEngine::onBytes);
    connect(m_transport, &TransportChannel::errorOccurred, this, &PollEngine::onTransportError);
    connect(m_transport, &TransportChannel::closed, this, &PollEngine::onTransportClosed);
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
    m_rx.clear();
    m_writeQueue.clear();
    m_pendingWrite = false;
    m_inFlight = false;
    m_failStreak = 0;
    m_backoffMs = 500;
    enterState(State::Connecting, QStringLiteral("开始连接 %1").arg(m_transport->describe()));
    if (!m_transport->open()) {
        scheduleReconnect();
        return;
    }
    // 串口 open() 是同步的；TCP 是异步的，所以统一用一个短周期观察 isOpen()
    m_connectClock.start();
    m_connectWatch.start(50);
}

void PollEngine::onConnectWatch()
{
    if (m_state != State::Connecting) {
        m_connectWatch.stop();
        return;
    }
    if (m_transport->isOpen()) {
        m_connectWatch.stop();
        enterState(State::Online, QStringLiteral("已连接 %1").arg(m_transport->describe()));
        m_tick.start(m_intervalMs);
        return;
    }
    if (m_connectClock.elapsed() > 3000) {
        m_connectWatch.stop();
        enterState(State::Backoff, QStringLiteral("连接超时（3 s）"));
        scheduleReconnect();
    }
}

void PollEngine::stop()
{
    m_tick.stop();
    m_deadline.stop();
    m_reconnect.stop();
    m_connectWatch.stop();
    m_transport->close();
    enterState(State::Stopped, QStringLiteral("已停止采集"));
}

bool PollEngine::enqueueWrite(uint16_t addr, uint16_t value)
{
    if (m_state != State::Online) {
        return false;
    }
    m_writeQueue.enqueue(qMakePair(addr, value));
    return true;
}

void PollEngine::onTick()
{
    if (m_state != State::Online || m_inFlight) {
        return;   // 有在途请求时不再发新请求，保证一问一答
    }
    sendNext();
}

void PollEngine::sendNext()
{
    m_rx.clear();
    m_inFlight = true;
    if (!m_writeQueue.isEmpty()) {
        const auto job = m_writeQueue.dequeue();
        m_pendingWrite = true;
        m_pendingAddr = job.first;
        m_pendingValue = job.second;
        const QByteArray frame = mb::buildWriteSingle(m_slave, job.first, job.second);
        emit frameLogged(QStringLiteral("TX"), frame,
                         QStringLiteral("写寄存器 %1 = %2").arg(job.first).arg(job.second));
        m_transport->write(frame);
        m_deadline.start(m_timeoutMs);
        return;
    }
    m_pendingWrite = false;
    const QByteArray frame = mb::buildReadHolding(m_slave, m_regBase, m_regCount);
    emit frameLogged(QStringLiteral("TX"), frame,
                     QStringLiteral("读寄存器 %1..%2").arg(m_regBase).arg(m_regBase + m_regCount - 1));
    m_transport->write(frame);
    m_deadline.start(m_timeoutMs);
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
    m_deadline.stop();
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
    m_tick.stop();
    m_transport->close();
    scheduleReconnect();
}

void PollEngine::scheduleReconnect()
{
    m_reconnect.start(m_backoffMs);
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
            m_tick.stop();
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
    m_tick.stop();
    scheduleReconnect();
}

void PollEngine::enterState(State s, const QString& detail)
{
    m_state = s;
    emit stateChanged(s, detail);
}
