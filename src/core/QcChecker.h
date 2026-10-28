#pragma once

#include <QMap>
#include <QString>
#include <QVector>

struct QcRow {
    QString device;
    QString batch;
    int kv = 0;
    int ma = 0;
    double ctCenter = 0.0;   // 水模中心 ROI 的 CT 值均值
    double ctEdge = 0.0;     // 水模边缘 ROI 的 CT 值均值
    double noise = 0.0;      // 噪声（ROI 内标准差）

    double uniformity() const;      // |中心 - 边缘|，单位 HU
    bool uniformOk(double limit) const { return uniformity() <= limit; }
    bool noiseOk(double limit) const { return noise <= limit; }
    bool pass(double uLimit, double nLimit) const { return uniformOk(uLimit) && noiseOk(nLimit); }
};

struct QcSummary {
    int total = 0;
    int passed = 0;
    int failed = 0;
    double maxUniformity = 0.0;
    double maxNoise = 0.0;
    QString worstDevice;        // 失败最多的一台
    QMap<QString, int> deviceFailures;
};

// 离线核对：读取设备测试导出表（device,batch,kv,ma,ct_center,ct_edge,noise），
// 按"CT 值均匀性（中心-边缘差）"与"噪声"两条指标判定合格与否，并生成核对报告。
// 对应实习里"人工导数据 → 逐组核对 → 标出异常组"的流程。
class QcChecker
{
public:
    bool load(const QString& path, QString* error);
    const QVector<QcRow>& rows() const { return m_rows; }

    void setUniformityLimit(double hu) { m_uniformityLimit = hu; }
    void setNoiseLimit(double hu) { m_noiseLimit = hu; }
    double uniformityLimit() const { return m_uniformityLimit; }
    double noiseLimit() const { return m_noiseLimit; }

    QcSummary summary() const;
    QString toMarkdown() const;
    bool exportMarkdown(const QString& path, QString* error) const;

private:
    QVector<QcRow> m_rows;
    double m_uniformityLimit = 8.0;   // HU
    double m_noiseLimit = 3.5;        // HU
};
