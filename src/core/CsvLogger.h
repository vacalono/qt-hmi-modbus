#pragma once

#include <QFile>
#include <QString>
#include <QTextStream>
#include <QVector>
#include <cstdint>

// 采集数据落盘：每次连接生成一个 CSV（csv_YYYYMMDD_HHmmss.csv），
// 一行一个采样点，带 ISO 时间戳与工程单位，方便用 Excel / 脚本二次分析。
// 现场软件里这一层通常还要做按天滚动、磁盘满保护，这里保持最小实现。
class CsvLogger
{
public:
    bool open(const QString& dir);
    void writeRow(qint64 timestampMs, const QVector<uint16_t>& regs, const QString& state);
    void close();

    bool isOpen() const { return m_file.isOpen(); }
    QString path() const { return m_file.fileName(); }
    int rows() const { return m_rows; }

private:
    QFile m_file;
    QTextStream m_stream;
    int m_rows = 0;
};
