#include "core/ScanController.h"

#include "core/PollEngine.h"

#include <QDateTime>
#include <QDir>

namespace {
constexpr int kRegSetKv = 6;
constexpr int kRegSetMa = 7;
}

ScanController::ScanController(PollEngine* engine, QObject* parent)
    : QObject(parent), m_engine(engine)
{
    m_settle.setSingleShot(true);
    connect(&m_settle, &QTimer::timeout, this, &ScanController::onSettled);
    connect(m_engine, &PollEngine::sample, this, &ScanController::onSample);
    connect(m_engine, &PollEngine::writeFinished, this, &ScanController::onWriteFinished);
}

void ScanController::setPlan(const QVector<int>& kvList, const QVector<int>& maList, int settleMs)
{
    m_plan.clear();
    for (int kv : kvList) {
        for (int ma : maList) {
            m_plan.append(qMakePair(kv, ma));
        }
    }
    m_settleMs = settleMs;
}

bool ScanController::start(const QString& csvDir)
{
    if (m_running || m_plan.isEmpty()) {
        return false;
    }
    QDir dir(csvDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        emit failed(QStringLiteral("无法创建输出目录：%1").arg(csvDir));
        return false;
    }
    m_path = dir.filePath(QStringLiteral("scan_%1.csv")
                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        emit failed(QStringLiteral("无法写入 %1").arg(m_path));
        return false;
    }
    m_out.setDevice(&m_file);
    m_out << "seq,set_kv,set_ma,meas_kv,meas_ma,tube_temp_c,dose_rate_ugy,calib_mm,status,timestamp\n";
    m_out.flush();

    m_index = 0;
    m_rows = 0;
    m_waitingForSample = false;
    m_pendingWrites = 0;
    m_running = true;
    nextCombo();
    return true;
}

void ScanController::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_settle.stop();
    finish(QStringLiteral("手动停止"));
}

void ScanController::nextCombo()
{
    if (!m_running) {
        return;
    }
    if (m_index >= m_plan.size()) {
        finish();
        return;
    }
    const auto combo = m_plan.at(m_index);
    m_pendingWrites = 2;
    m_waitingForSample = false;
    emit progress(m_index, m_plan.size(),
                  QStringLiteral("下发 kV=%1 / mA=%2").arg(combo.first).arg(combo.second));
    m_engine->enqueueWrite(kRegSetKv, static_cast<uint16_t>(combo.first));
    m_engine->enqueueWrite(kRegSetMa, static_cast<uint16_t>(combo.second));
}

void ScanController::onWriteFinished(bool ok, uint16_t addr, uint16_t value, const QString& detail)
{
    if (!m_running || (addr != kRegSetKv && addr != kRegSetMa)) {
        return;
    }
    if (!ok) {
        finish(QStringLiteral("写寄存器失败（HR%1=%2）：%3").arg(addr).arg(value).arg(detail));
        return;
    }
    if (--m_pendingWrites <= 0) {
        m_pendingWrites = 0;
        m_settle.start(m_settleMs);   // 两个设定值都写成功，等读数稳定
    }
}

void ScanController::onSettled()
{
    m_waitingForSample = true;   // 下一帧采样即作为该组合的记录值
}

void ScanController::onSample(const QVector<uint16_t>& regs, qint64 timestampMs)
{
    if (!m_running) {
        return;
    }
    m_lastRegs = regs;
    m_lastTs = timestampMs;
    if (!m_waitingForSample) {
        return;
    }
    m_waitingForSample = false;

    auto reg = [this](int i) -> uint16_t { return i < m_lastRegs.size() ? m_lastRegs.at(i) : 0; };
    const auto combo = m_plan.at(m_index);
    const QString ts = QDateTime::fromMSecsSinceEpoch(m_lastTs)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    m_out << (m_index + 1) << ',' << combo.first << ',' << combo.second
          << ',' << reg(1) << ',' << reg(2)
          << ',' << QString::number(reg(0) / 10.0, 'f', 1)
          << ',' << QString::number(reg(5) / 10.0, 'f', 1)
          << ',' << QString::number(reg(4) / 100.0, 'f', 2)
          << ',' << QStringLiteral("0x%1").arg(reg(3), 4, 16, QLatin1Char('0'))
          << ',' << ts << '\n';
    m_out.flush();
    ++m_rows;

    ++m_index;
    emit progress(m_index, m_plan.size(), QStringLiteral("已记录 %1 / %2 组").arg(m_index).arg(m_plan.size()));
    nextCombo();
}

void ScanController::finish(const QString& why)
{
    m_running = false;
    m_settle.stop();
    if (m_file.isOpen()) {
        m_out.flush();
        m_file.close();
    }
    if (!why.isEmpty()) {
        emit failed(why);
    }
    emit finished(m_path, m_rows);
}
