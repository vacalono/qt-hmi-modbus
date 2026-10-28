#include "core/CsvLogger.h"

#include <QDateTime>
#include <QDir>

bool CsvLogger::open(const QString& dir)
{
    QDir d(dir);
    if (!d.exists() && !d.mkpath(QStringLiteral("."))) {
        return false;
    }
    const QString name = QStringLiteral("csv_%1.csv")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    m_file.setFileName(d.filePath(name));
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    m_stream.setDevice(&m_file);
    m_stream << "timestamp,tube_temp_c,kv,ma,status,calib_mm,dose_rate_ugy,set_kv,set_ma,state\n";
    m_stream.flush();
    m_rows = 0;
    return true;
}

void CsvLogger::writeRow(qint64 timestampMs, const QVector<uint16_t>& regs, const QString& state)
{
    if (!m_file.isOpen()) {
        return;
    }
    auto reg = [&regs](int i) -> uint16_t { return i < regs.size() ? regs.at(i) : 0; };

    const QString ts = QDateTime::fromMSecsSinceEpoch(timestampMs)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const double tubeTempC = reg(0) / 10.0;     // HR0：球管温度 ×10
    const int kv = reg(1);                      // HR1：实测 kV
    const int ma = reg(2);                      // HR2：实测 mA
    const uint16_t status = reg(3);             // HR3：状态位
    const double calibMm = reg(4) / 100.0;      // HR4：校准片偏移 ×100
    const double dose = reg(5) / 10.0;          // HR5：剂量率 ×10
    const int setKv = reg(6);                   // HR6：设定 kV
    const int setMa = reg(7);                   // HR7：设定 mA

    m_stream << ts << ',' << QString::number(tubeTempC, 'f', 1) << ',' << kv << ',' << ma
             << ',' << QStringLiteral("0x%1").arg(status, 4, 16, QLatin1Char('0'))
             << ',' << QString::number(calibMm, 'f', 2) << ',' << QString::number(dose, 'f', 1)
             << ',' << setKv << ',' << setMa << ',' << state << '\n';
    m_stream.flush();   // 采样周期 300ms，直接刷盘；真实项目会做批量写
    ++m_rows;
}

void CsvLogger::close()
{
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }
}
