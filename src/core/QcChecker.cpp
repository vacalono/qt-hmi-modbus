#include "core/QcChecker.h"

#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QtMath>

double QcRow::uniformity() const
{
    return qAbs(ctCenter - ctEdge);
}

bool QcChecker::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("打不开文件：%1").arg(path);
        }
        return false;
    }
    QTextStream in(&f);
    const QString header = in.readLine().trimmed();
    const QStringList cols = header.split(QLatin1Char(','));
    // 列顺序不写死，按表头找；缺列直接报错，避免把错列算成指标
    auto idx = [&cols](const QString& name) { return cols.indexOf(name); };
    const int iDevice = idx(QStringLiteral("device"));
    const int iBatch = idx(QStringLiteral("batch"));
    const int iKv = idx(QStringLiteral("kv"));
    const int iMa = idx(QStringLiteral("ma"));
    const int iCenter = idx(QStringLiteral("ct_center"));
    const int iEdge = idx(QStringLiteral("ct_edge"));
    const int iNoise = idx(QStringLiteral("noise"));
    if (iDevice < 0 || iKv < 0 || iMa < 0 || iCenter < 0 || iEdge < 0 || iNoise < 0) {
        if (error) {
            *error = QStringLiteral("表头缺列，需要 device,kv,ma,ct_center,ct_edge,noise");
        }
        return false;
    }

    m_rows.clear();
    int lineNo = 1;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        ++lineNo;
        if (line.isEmpty()) {
            continue;
        }
        const QStringList f2 = line.split(QLatin1Char(','));
        if (f2.size() <= qMax(iEdge, iNoise)) {
            continue;   // 残缺行跳过（导出工具写一半的情况）
        }
        QcRow r;
        r.device = f2.at(iDevice).trimmed();
        r.batch = iBatch >= 0 ? f2.at(iBatch).trimmed() : QString();
        r.kv = f2.at(iKv).toInt();
        r.ma = f2.at(iMa).toInt();
        r.ctCenter = f2.at(iCenter).toDouble();
        r.ctEdge = f2.at(iEdge).toDouble();
        r.noise = f2.at(iNoise).toDouble();
        m_rows.append(r);
    }
    if (m_rows.isEmpty()) {
        if (error) {
            *error = QStringLiteral("文件里没有有效数据行");
        }
        return false;
    }
    return true;
}

QcSummary QcChecker::summary() const
{
    QcSummary s;
    s.total = m_rows.size();
    for (const QcRow& r : m_rows) {
        if (r.pass(m_uniformityLimit, m_noiseLimit)) {
            ++s.passed;
        } else {
            ++s.failed;
            s.deviceFailures[r.device] += 1;
        }
        s.maxUniformity = qMax(s.maxUniformity, r.uniformity());
        s.maxNoise = qMax(s.maxNoise, r.noise);
    }
    int worst = 0;
    for (auto it = s.deviceFailures.constBegin(); it != s.deviceFailures.constEnd(); ++it) {
        if (it.value() > worst) {
            worst = it.value();
            s.worstDevice = it.key();
        }
    }
    return s;
}

QString QcChecker::toMarkdown() const
{
    const QcSummary s = summary();
    QString out;
    QTextStream ts(&out);
    ts << "# 图像质量测试核对报告\n\n";
    ts << QStringLiteral("- 生成时间：%1\n")
              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    ts << QStringLiteral("- 判定阈值：CT 值均匀性 ≤ %1 HU，噪声 ≤ %2 HU\n")
              .arg(m_uniformityLimit, 0, 'f', 1)
              .arg(m_noiseLimit, 0, 'f', 1);
    ts << QStringLiteral("- 总数：**%1** 组 ｜ 合格 **%2** ｜ 不合格 **%3** ｜ 通过率 %4%\n\n")
              .arg(s.total)
              .arg(s.passed)
              .arg(s.failed)
              .arg(s.total > 0 ? QString::number(100.0 * s.passed / s.total, 'f', 1) : QStringLiteral("0"));
    ts << QStringLiteral("- 最差项：均匀性最大 %1 HU，噪声最大 %2 HU\n").arg(s.maxUniformity, 0, 'f', 2).arg(s.maxNoise, 0, 'f', 2);
    if (!s.worstDevice.isEmpty()) {
        ts << QStringLiteral("- 不合格集中在：%1（%2 组）\n").arg(s.worstDevice).arg(s.deviceFailures.value(s.worstDevice));
    }

    ts << "\n## 不合格明细\n\n";
    ts << "| # | 设备 | 批次 | kV | mA | 中心 CT | 边缘 CT | 均匀性 | 噪声 | 判定原因 |\n";
    ts << "|---|---|---|---|---|---|---|---|---|---|\n";
    int n = 0;
    for (const QcRow& r : m_rows) {
        if (r.pass(m_uniformityLimit, m_noiseLimit)) {
            continue;
        }
        ++n;
        QStringList why;
        if (!r.uniformOk(m_uniformityLimit)) {
            why << QStringLiteral("均匀性超限");
        }
        if (!r.noiseOk(m_noiseLimit)) {
            why << QStringLiteral("噪声超限");
        }
        ts << QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 |\n")
                  .arg(n).arg(r.device, r.batch).arg(r.kv).arg(r.ma)
                  .arg(r.ctCenter, 0, 'f', 1).arg(r.ctEdge, 0, 'f', 1)
                  .arg(r.uniformity(), 0, 'f', 2).arg(r.noise, 0, 'f', 2)
                  .arg(why.join(QStringLiteral("+")));
    }
    if (n == 0) {
        ts << "| — | 无 | | | | | | | | 全部合格 |\n";
    }
    return out;
}

bool QcChecker::exportMarkdown(const QString& path, QString* error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("写不了报告文件：%1").arg(path);
        }
        return false;
    }
    QTextStream out(&f);
    out << toMarkdown();
    return true;
}
