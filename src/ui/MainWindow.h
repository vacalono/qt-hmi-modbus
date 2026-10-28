#pragma once

#include <QMainWindow>
#include <QVector>
#include <memory>

#include "core/PollEngine.h"
#include "core/QcChecker.h"

class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QTableWidget;
class QPlainTextEdit;
class QLabel;
class QChartView;
class QLineSeries;
class QChart;

class TransportChannel;
class PollEngine;
class CsvLogger;
class ScanController;
class QProgressBar;
class QDoubleSpinBox;
class QTabWidget;

// 上位机主界面：连接参数 / 实时数据表 / 趋势曲线 / 参数下发 / 通信日志。
// 采集与协议都在 core 层，这里只负责"显示 + 操作"，不碰字节流。
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 无头联调用：设置连接参数后直接连，便于 CI / 冒烟脚本验证
    void prepareAutoConnect(const QString& host, quint16 port, int intervalMs);
    void connectNow();
    QString csvPath() const;
    int csvRows() const;
    quint64 okFrames() const;
    quint64 timeouts() const;
    quint64 crcErrors() const;
    quint64 retries() const;
    // 无头冒烟用：直接跑一段测试序列
    bool startScan(const QVector<int>& kvList, const QVector<int>& maList);
    int scanRows() const;
    QString scanPath() const;
    // 离线核对（CLI 与界面共用）
    bool loadQcCsv(const QString& path);
    QString qcMarkdown() const;
    bool exportQcReport(const QString& path, QString* error) const;
    int qcTotal() const { return m_qc.summary().total; }
    int qcPassed() const { return m_qc.summary().passed; }
    int qcFailed() const { return m_qc.summary().failed; }
    void selectTab(int index);
    quintptr ioThreadId() const;
    quint64 engineTicks() const;
    double engineLatency() const;

private slots:
    void onConnectClicked();
    void onSample(const QVector<uint16_t>& regs, qint64 timestampMs);
    void onStateChanged(PollEngine::State state, const QString& detail);
    void onFrameLogged(const QString& dir, const QByteArray& frame, const QString& note);
    void onWriteClicked();
    void onWriteFinished(bool ok, uint16_t addr, uint16_t value, const QString& detail);
    void onScanClicked();
    void onScanStopClicked();

private:
    QWidget* buildOnlineTab();
    QWidget* buildOfflineTab();
    QWidget* buildConnectPanel();
    QWidget* buildDataPanel();
    QWidget* buildChartPanel();
    QWidget* buildWritePanel();
    QWidget* buildScanPanel();
    QWidget* buildLogPanel();
    void appendLog(const QString& text);
    void updateCounterLabel();
    void refreshQcTable();
    void setConfigEnabled(bool enabled);

    // 连接区
    QComboBox* m_transportBox = nullptr;
    QLineEdit* m_hostEdit = nullptr;
    QSpinBox* m_portSpin = nullptr;
    QComboBox* m_serialPortBox = nullptr;
    QComboBox* m_baudBox = nullptr;
    QSpinBox* m_slaveSpin = nullptr;
    QSpinBox* m_intervalSpin = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QLabel* m_stateLabel = nullptr;

    // 数据 / 曲线
    QTableWidget* m_table = nullptr;
    QChartView* m_chartView = nullptr;
    QChart* m_chart = nullptr;
    QLineSeries* m_tempSeries = nullptr;
    QLineSeries* m_doseSeries = nullptr;

    // 下发
    QSpinBox* m_writeAddrSpin = nullptr;
    QSpinBox* m_writeValueSpin = nullptr;
    QPushButton* m_writeBtn = nullptr;

    // 日志 / 状态栏
    QPlainTextEdit* m_log = nullptr;
    QLabel* m_counterLabel = nullptr;

    // 测试序列
    QSpinBox* m_kvFrom = nullptr;
    QSpinBox* m_kvTo = nullptr;
    QSpinBox* m_kvStep = nullptr;
    QLineEdit* m_maList = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QPushButton* m_scanStopBtn = nullptr;
    QProgressBar* m_scanBar = nullptr;
    QLabel* m_scanLabel = nullptr;

    // 离线核对
    QcChecker m_qc;
    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_qcTable = nullptr;
    QLabel* m_qcSummary = nullptr;
    QDoubleSpinBox* m_uLimit = nullptr;
    QDoubleSpinBox* m_nLimit = nullptr;
    QPushButton* m_loadBtn = nullptr;
    QPushButton* m_exportBtn = nullptr;

    TransportChannel* m_channel = nullptr;
    PollEngine* m_engine = nullptr;
    ScanController* m_scan = nullptr;
    std::unique_ptr<CsvLogger> m_csv;
    qint64 m_pointIndex = 0;
    int m_chartWindow = 300;
};
