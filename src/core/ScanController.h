#pragma once

#include <QFile>
#include <QObject>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <cstdint>

class PollEngine;

// 测试序列：按 kV × mA 矩阵逐组下发参数 → 等读数稳定 → 记录一组数据。
// 对应实习里"5 台 × 127 组"那套人工流程的自动化版本：
// 人工是改参数、等稳定、抄数据；这里是写寄存器、等 settling、落盘。
class ScanController : public QObject
{
    Q_OBJECT
public:
    explicit ScanController(PollEngine* engine, QObject* parent = nullptr);

    void setPlan(const QVector<int>& kvList, const QVector<int>& maList, int settleMs = 900);

    bool start(const QString& csvDir);
    void stop();
    bool isRunning() const { return m_running; }
    QString csvPath() const { return m_path; }
    int rows() const { return m_rows; }

signals:
    void progress(int done, int total, const QString& detail);
    void finished(const QString& csvPath, int rows);
    void failed(const QString& why);

private slots:
    void onWriteFinished(bool ok, uint16_t addr, uint16_t value, const QString& detail);
    void onSettled();
    void onSample(const QVector<uint16_t>& regs, qint64 timestampMs);

private:
    void nextCombo();
    void finish(const QString& why = QString());

    PollEngine* m_engine = nullptr;
    QVector<QPair<int, int>> m_plan;   // (kV, mA)
    int m_index = 0;
    int m_settleMs = 900;
    bool m_running = false;
    bool m_waitingForSample = false;
    int m_pendingWrites = 0;

    QFile m_file;
    QTextStream m_out;
    QString m_path;
    int m_rows = 0;

    QTimer m_settle;
    QVector<uint16_t> m_lastRegs;
    qint64 m_lastTs = 0;
};
