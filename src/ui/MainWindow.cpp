#include "ui/MainWindow.h"

#include "core/CsvLogger.h"
#include "core/PollEngine.h"
#include "core/ScanController.h"
#include "transport/SerialTransport.h"
#include "transport/TransportChannel.h"

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>

namespace {
constexpr int kRegCount = 8;      // HR0..HR7
constexpr int kRegSetKv = 6;      // 设定 kV
constexpr int kRegSetMa = 7;      // 设定 mA
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("上位机采集监控 · Modbus RTU（Qt6 / C++17）"));
    resize(1180, 760);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildOnlineTab(), QStringLiteral("在线采集"));
    tabs->addTab(buildOfflineTab(), QStringLiteral("离线核对（导入导出数据）"));
    setCentralWidget(tabs);
    m_tabs = tabs;
    m_tabs->setObjectName(QStringLiteral("mainTabs"));          // UI 自动化用

    m_counterLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_counterLabel);
    updateCounterLabel();
    appendLog(QStringLiteral("就绪。先启动 device_sim，再点「连接」。默认 TCP 127.0.0.1:15020（RTU over TCP）。"));
}

MainWindow::~MainWindow()
{
    if (m_engine) {
        m_engine->stop();
    }
    if (m_csv) {
        m_csv->close();
    }
}

QWidget* MainWindow::buildOnlineTab()
{
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    root->addWidget(buildConnectPanel());

    auto* middle = new QHBoxLayout();
    middle->addWidget(buildDataPanel(), 2);
    middle->addWidget(buildChartPanel(), 3);
    root->addLayout(middle, 1);

    root->addWidget(buildScanPanel());

    auto* bottomRow = new QHBoxLayout();
    bottomRow->addWidget(buildWritePanel(), 1);
    bottomRow->addWidget(buildLogPanel(), 3);
    root->addLayout(bottomRow, 1);
    return page;
}

QWidget* MainWindow::buildOfflineTab()
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);

    auto* row = new QHBoxLayout();
    m_loadBtn = new QPushButton(QStringLiteral("选择导出的测试数据 CSV…"), page);
    m_uLimit = new QDoubleSpinBox(page);
    m_uLimit->setRange(0.5, 50.0);
    m_uLimit->setSingleStep(0.5);
    m_uLimit->setValue(8.0);
    m_uLimit->setSuffix(QStringLiteral(" HU"));
    m_nLimit = new QDoubleSpinBox(page);
    m_nLimit->setRange(0.1, 20.0);
    m_nLimit->setSingleStep(0.1);
    m_nLimit->setValue(3.5);
    m_nLimit->setSuffix(QStringLiteral(" HU"));
    m_exportBtn = new QPushButton(QStringLiteral("导出核对报告"), page);
    m_exportBtn->setEnabled(false);

    row->addWidget(m_loadBtn);
    row->addWidget(new QLabel(QStringLiteral("均匀性 ≤"), page));
    row->addWidget(m_uLimit);
    row->addWidget(new QLabel(QStringLiteral("噪声 ≤"), page));
    row->addWidget(m_nLimit);
    row->addWidget(m_exportBtn);
    row->addStretch();
    v->addLayout(row);

    m_qcTable = new QTableWidget(0, 9, page);
    m_qcTable->setHorizontalHeaderLabels({QStringLiteral("设备"), QStringLiteral("批次"), QStringLiteral("kV"),
                                          QStringLiteral("mA"), QStringLiteral("中心 CT"), QStringLiteral("边缘 CT"),
                                          QStringLiteral("均匀性 HU"), QStringLiteral("噪声 HU"), QStringLiteral("判定")});
    m_qcTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_qcTable->verticalHeader()->setVisible(false);
    m_qcTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->addWidget(m_qcTable, 1);

    m_qcSummary = new QLabel(QStringLiteral("尚未导入数据。表头需包含 device,batch,kv,ma,ct_center,ct_edge,noise。"), page);
    v->addWidget(m_qcSummary);

    connect(m_loadBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择测试数据 CSV"), QString(),
                                                          QStringLiteral("CSV (*.csv)"));
        if (!path.isEmpty()) {
            loadQcCsv(path);
        }
    });
    connect(m_exportBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存核对报告"),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs/qc_report.md")),
            QStringLiteral("Markdown (*.md)"));
        if (path.isEmpty()) {
            return;
        }
        QString err;
        if (exportQcReport(path, &err)) {
            m_qcSummary->setText(m_qcSummary->text() + QStringLiteral("　｜　报告：%1").arg(path));
        } else {
            m_qcSummary->setText(err);
        }
    });
    connect(m_uLimit, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_qc.setUniformityLimit(v);
        refreshQcTable();
    });
    connect(m_nLimit, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_qc.setNoiseLimit(v);
        refreshQcTable();
    });
    return page;
}

quintptr MainWindow::ioThreadId() const
{
    return m_channel ? m_channel->ioThreadId() : 0;
}

quint64 MainWindow::engineTicks() const
{
    return m_engine ? m_engine->ticks() : 0;
}

double MainWindow::engineLatency() const
{
    return m_engine ? m_engine->lastPollLatencyMs() : 0.0;
}

void MainWindow::selectTab(int index)
{
    if (m_tabs) {
        m_tabs->setCurrentIndex(index);
    }
}

bool MainWindow::loadQcCsv(const QString& path)
{
    QString err;
    if (!m_qc.load(path, &err)) {
        if (m_qcSummary) {
            m_qcSummary->setText(err);
        }
        return false;
    }
    m_qc.setUniformityLimit(m_uLimit ? m_uLimit->value() : 8.0);
    m_qc.setNoiseLimit(m_nLimit ? m_nLimit->value() : 3.5);
    if (m_exportBtn) {
        m_exportBtn->setEnabled(true);
    }
    refreshQcTable();
    if (m_qcSummary) {
        m_qcSummary->setText(m_qcSummary->text() + QStringLiteral("　｜　来源：%1").arg(QFileInfo(path).fileName()));
    }
    return true;
}

void MainWindow::refreshQcTable()
{
    if (!m_qcTable) {
        return;
    }
    const auto& rows = m_qc.rows();
    m_qcTable->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const QcRow& r = rows.at(i);
        const bool ok = r.pass(m_qc.uniformityLimit(), m_qc.noiseLimit());
        const QStringList cells = {
            r.device, r.batch, QString::number(r.kv), QString::number(r.ma),
            QString::number(r.ctCenter, 'f', 1), QString::number(r.ctEdge, 'f', 1),
            QString::number(r.uniformity(), 'f', 2), QString::number(r.noise, 'f', 2),
            ok ? QStringLiteral("合格") : QStringLiteral("不合格")
        };
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells.at(c));
            if (!ok) {
                item->setBackground(QColor(255, 235, 235));
                item->setForeground(QColor(170, 0, 0));
            }
            m_qcTable->setItem(i, c, item);
        }
    }
    const QcSummary s = m_qc.summary();
    if (m_qcSummary) {
        QString text = QStringLiteral("共 %1 组 ｜ 合格 %2 ｜ 不合格 %3 ｜ 通过率 %4%　｜　均匀性最大 %5 HU，噪声最大 %6 HU")
                           .arg(s.total).arg(s.passed).arg(s.failed)
                           .arg(s.total ? QString::number(100.0 * s.passed / s.total, 'f', 1) : QStringLiteral("0"))
                           .arg(s.maxUniformity, 0, 'f', 2).arg(s.maxNoise, 0, 'f', 2);
        if (!s.worstDevice.isEmpty()) {
            text += QStringLiteral("　｜　不合格集中在 %1（%2 组）").arg(s.worstDevice).arg(s.deviceFailures.value(s.worstDevice));
        }
        m_qcSummary->setText(text);
    }
}

QString MainWindow::qcMarkdown() const
{
    return m_qc.toMarkdown();
}

bool MainWindow::exportQcReport(const QString& path, QString* error) const
{
    return m_qc.exportMarkdown(path, error);
}

QWidget* MainWindow::buildConnectPanel()
{
    auto* box = new QGroupBox(QStringLiteral("连接"), this);
    auto* h = new QHBoxLayout(box);

    m_transportBox = new QComboBox(box);
    m_transportBox->addItem(QStringLiteral("TCP（RTU over TCP）"));
    m_transportBox->addItem(QStringLiteral("串口"));

    m_hostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), box);
    m_hostEdit->setMaximumWidth(120);

    m_portSpin = new QSpinBox(box);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(15020);

    m_serialPortBox = new QComboBox(box);
    m_serialPortBox->addItems(SerialTransport::availablePorts());
    m_serialPortBox->setMinimumWidth(90);
    m_serialPortBox->setVisible(false);

    m_baudBox = new QComboBox(box);
    for (int b : {9600, 19200, 38400, 57600, 115200}) {
        m_baudBox->addItem(QString::number(b), b);
    }
    m_baudBox->setCurrentText(QStringLiteral("115200"));
    m_baudBox->setVisible(false);

    m_slaveSpin = new QSpinBox(box);
    m_slaveSpin->setRange(1, 247);
    m_slaveSpin->setValue(1);
    m_slaveSpin->setPrefix(QStringLiteral("从站 "));

    m_intervalSpin = new QSpinBox(box);
    m_intervalSpin->setObjectName(QStringLiteral("intervalSpin"));
    m_intervalSpin->setRange(50, 5000);
    m_intervalSpin->setSingleStep(50);
    m_intervalSpin->setValue(300);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));

    m_connectBtn = new QPushButton(QStringLiteral("连接"), box);
    m_connectBtn->setObjectName(QStringLiteral("connectButton"));   // UI 自动化用
    m_stateLabel = new QLabel(QStringLiteral("● 未连接"), box);
    m_stateLabel->setObjectName(QStringLiteral("stateLabel"));
    m_stateLabel->setMinimumWidth(220);

    h->addWidget(new QLabel(QStringLiteral("通道"), box));
    h->addWidget(m_transportBox);
    h->addWidget(m_hostEdit);
    h->addWidget(m_portSpin);
    h->addWidget(m_serialPortBox);
    h->addWidget(m_baudBox);
    h->addWidget(m_slaveSpin);
    h->addWidget(new QLabel(QStringLiteral("周期"), box));
    h->addWidget(m_intervalSpin);
    h->addWidget(m_connectBtn);
    h->addWidget(m_stateLabel, 1);

    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_transportBox, &QComboBox::currentIndexChanged, this, [this](int idx) {
        const bool serial = (idx == 1);
        m_hostEdit->setVisible(!serial);
        m_portSpin->setVisible(!serial);
        m_serialPortBox->setVisible(serial);
        m_baudBox->setVisible(serial);
    });
    return box;
}

QWidget* MainWindow::buildDataPanel()
{
    auto* box = new QGroupBox(QStringLiteral("实时数据（保持寄存器 0x03）"), this);
    auto* v = new QVBoxLayout(box);

    m_table = new QTableWidget(kRegCount, 3, box);
    m_table->setHorizontalHeaderLabels({QStringLiteral("寄存器"), QStringLiteral("数值"), QStringLiteral("单位")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QStringList names = {
        QStringLiteral("HR0 球管温度"), QStringLiteral("HR1 实测 kV"), QStringLiteral("HR2 实测 mA"),
        QStringLiteral("HR3 状态位"), QStringLiteral("HR4 校准片偏移"), QStringLiteral("HR5 剂量率"),
        QStringLiteral("HR6 设定 kV"), QStringLiteral("HR7 设定 mA")
    };
    const QStringList units = {QStringLiteral("℃"), QStringLiteral("kV"), QStringLiteral("mA"),
                               QStringLiteral("位域"), QStringLiteral("mm"), QStringLiteral("µGy/s"),
                               QStringLiteral("kV"), QStringLiteral("mA")};
    for (int r = 0; r < names.size(); ++r) {
        m_table->setItem(r, 0, new QTableWidgetItem(names.at(r)));
        m_table->setItem(r, 1, new QTableWidgetItem(QStringLiteral("—")));
        m_table->setItem(r, 2, new QTableWidgetItem(units.at(r)));
    }
    v->addWidget(m_table);
    return box;
}

QWidget* MainWindow::buildChartPanel()
{
    auto* box = new QGroupBox(QStringLiteral("趋势（球管温度 ℃ / 剂量率 µGy/s）"), this);
    auto* v = new QVBoxLayout(box);

    m_tempSeries = new QLineSeries();
    m_tempSeries->setName(QStringLiteral("球管温度 ℃"));
    m_doseSeries = new QLineSeries();
    m_doseSeries->setName(QStringLiteral("剂量率 µGy/s"));

    m_chart = new QChart();
    m_chart->addSeries(m_tempSeries);
    m_chart->addSeries(m_doseSeries);
    m_chart->legend()->setVisible(true);
    m_chart->setAnimationOptions(QChart::NoAnimation);   // 300ms 刷新，开动画反而掉帧

    auto* axisX = new QValueAxis();
    axisX->setRange(0, m_chartWindow);
    axisX->setLabelFormat(QStringLiteral("%d"));
    axisX->setTitleText(QStringLiteral("采样点"));

    auto* axisTemp = new QValueAxis();
    axisTemp->setRange(0, 100);
    axisTemp->setTitleText(QStringLiteral("温度 ℃"));

    auto* axisDose = new QValueAxis();
    axisDose->setRange(0, 800);
    axisDose->setTitleText(QStringLiteral("剂量率 µGy/s"));

    m_chart->addAxis(axisX, Qt::AlignBottom);
    m_chart->addAxis(axisTemp, Qt::AlignLeft);
    m_chart->addAxis(axisDose, Qt::AlignRight);
    m_tempSeries->attachAxis(axisX);
    m_tempSeries->attachAxis(axisTemp);
    m_doseSeries->attachAxis(axisX);
    m_doseSeries->attachAxis(axisDose);

    m_chartView = new QChartView(m_chart, box);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    v->addWidget(m_chartView);
    return box;
}

QWidget* MainWindow::buildWritePanel()
{
    auto* box = new QGroupBox(QStringLiteral("参数下发（0x06 写单寄存器）"), this);
    auto* v = new QVBoxLayout(box);

    auto* row = new QHBoxLayout();
    m_writeAddrSpin = new QSpinBox(box);
    m_writeAddrSpin->setRange(0, kRegCount - 1);
    m_writeAddrSpin->setValue(kRegSetKv);
    m_writeAddrSpin->setPrefix(QStringLiteral("HR"));
    m_writeValueSpin = new QSpinBox(box);
    m_writeValueSpin->setRange(0, 65535);
    m_writeValueSpin->setValue(120);
    m_writeBtn = new QPushButton(QStringLiteral("下发"), box);
    m_writeBtn->setObjectName(QStringLiteral("writeButton"));

    row->addWidget(m_writeAddrSpin);
    row->addWidget(new QLabel(QStringLiteral("="), box));
    row->addWidget(m_writeValueSpin);
    row->addWidget(m_writeBtn);
    v->addLayout(row);
    v->addWidget(new QLabel(QStringLiteral("写请求会排在采集请求之后（同一请求槽串行），回显不一致会报错。"), box));
    v->addStretch();

    connect(m_writeBtn, &QPushButton::clicked, this, &MainWindow::onWriteClicked);
    return box;
}

QWidget* MainWindow::buildScanPanel()
{
    auto* box = new QGroupBox(QStringLiteral("测试序列（kV × mA 矩阵，自动记录）"), this);
    auto* h = new QHBoxLayout(box);

    m_kvFrom = new QSpinBox(box);
    m_kvFrom->setRange(40, 200);
    m_kvFrom->setValue(80);
    m_kvFrom->setSuffix(QStringLiteral(" kV"));
    m_kvTo = new QSpinBox(box);
    m_kvTo->setRange(40, 200);
    m_kvTo->setValue(140);
    m_kvTo->setSuffix(QStringLiteral(" kV"));
    m_kvStep = new QSpinBox(box);
    m_kvStep->setRange(5, 60);
    m_kvStep->setSingleStep(5);
    m_kvStep->setValue(20);
    m_kvStep->setPrefix(QStringLiteral("步长 "));

    m_maList = new QLineEdit(QStringLiteral("100,200,300"), box);
    m_maList->setMaximumWidth(160);

    m_scanBtn = new QPushButton(QStringLiteral("开始序列"), box);
    m_scanBtn->setObjectName(QStringLiteral("scanButton"));
    m_scanStopBtn = new QPushButton(QStringLiteral("停止"), box);
    m_scanStopBtn->setObjectName(QStringLiteral("scanStopButton"));
    m_scanStopBtn->setEnabled(false);
    m_scanBar = new QProgressBar(box);
    m_scanBar->setRange(0, 100);
    m_scanBar->setValue(0);
    m_scanBar->setMaximumWidth(200);
    m_scanLabel = new QLabel(QStringLiteral("未开始"), box);
    m_scanLabel->setMinimumWidth(200);

    h->addWidget(new QLabel(QStringLiteral("kV 从"), box));
    h->addWidget(m_kvFrom);
    h->addWidget(new QLabel(QStringLiteral("到"), box));
    h->addWidget(m_kvTo);
    h->addWidget(m_kvStep);
    h->addWidget(new QLabel(QStringLiteral("mA 列表"), box));
    h->addWidget(m_maList);
    h->addWidget(m_scanBtn);
    h->addWidget(m_scanStopBtn);
    h->addWidget(m_scanBar);
    h->addWidget(m_scanLabel, 1);

    connect(m_scanBtn, &QPushButton::clicked, this, &MainWindow::onScanClicked);
    connect(m_scanStopBtn, &QPushButton::clicked, this, &MainWindow::onScanStopClicked);
    return box;
}

void MainWindow::onScanClicked()
{
    if (!m_scan || !m_engine || m_engine->state() != PollEngine::State::Online) {
        appendLog(QStringLiteral("未在线，测试序列无法开始"));
        return;
    }
    QVector<int> kvList;
    for (int kv = m_kvFrom->value(); kv <= m_kvTo->value(); kv += m_kvStep->value()) {
        kvList.append(kv);
    }
    QVector<int> maList;
    for (const QString& part : m_maList->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int ma = part.trimmed().toInt(&ok);
        if (ok) {
            maList.append(ma);
        }
    }
    if (kvList.isEmpty() || maList.isEmpty()) {
        appendLog(QStringLiteral("测试序列参数为空"));
        return;
    }
    if (!startScan(kvList, maList)) {
        appendLog(QStringLiteral("测试序列启动失败"));
    }
}

void MainWindow::onScanStopClicked()
{
    if (m_scan) {
        m_scan->stop();
    }
}

bool MainWindow::startScan(const QVector<int>& kvList, const QVector<int>& maList)
{
    if (!m_scan) {
        return false;
    }
    m_scan->setPlan(kvList, maList, 900);
    m_scanBar->setRange(0, kvList.size() * maList.size());
    m_scanBar->setValue(0);
    if (!m_scan->start(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs")))) {
        return false;
    }
    m_scanBtn->setEnabled(false);
    m_scanStopBtn->setEnabled(true);
    appendLog(QStringLiteral("测试序列开始：%1 组（kV %2 个 × mA %3 个）")
                  .arg(kvList.size() * maList.size()).arg(kvList.size()).arg(maList.size()));
    return true;
}

int MainWindow::scanRows() const
{
    return m_scan ? m_scan->rows() : 0;
}

QString MainWindow::scanPath() const
{
    return m_scan ? m_scan->csvPath() : QString();
}

QWidget* MainWindow::buildLogPanel()
{
    auto* box = new QGroupBox(QStringLiteral("通信日志"), this);
    auto* v = new QVBoxLayout(box);
    m_log = new QPlainTextEdit(box);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(800);
    m_log->setStyleSheet(QStringLiteral("font-family: Consolas, monospace;"));
    v->addWidget(m_log);
    return box;
}

void MainWindow::prepareAutoConnect(const QString& host, quint16 port, int intervalMs)
{
    m_transportBox->setCurrentIndex(0);   // TCP
    m_hostEdit->setText(host);
    m_portSpin->setValue(port);
    m_intervalSpin->setValue(intervalMs);
}

void MainWindow::connectNow()
{
    if (!m_engine) {
        onConnectClicked();
    }
}

QString MainWindow::csvPath() const
{
    return m_csv ? m_csv->path() : QString();
}

int MainWindow::csvRows() const
{
    return m_csv ? m_csv->rows() : 0;
}

quint64 MainWindow::okFrames() const
{
    return m_engine ? m_engine->okFrames() : 0;
}

quint64 MainWindow::timeouts() const
{
    return m_engine ? m_engine->timeouts() : 0;
}

quint64 MainWindow::crcErrors() const
{
    return m_engine ? m_engine->crcErrors() : 0;
}

quint64 MainWindow::retries() const
{
    return m_engine ? m_engine->retries() : 0;
}

void MainWindow::onConnectClicked()
{
    if (m_engine) {   // 断开
        if (m_scan) {
            m_scan->stop();
            delete m_scan;
            m_scan = nullptr;
        }
        m_scanBtn->setEnabled(true);
        m_scanStopBtn->setEnabled(false);
        QMetaObject::invokeMethod(m_engine, "stop", Qt::BlockingQueuedConnection);
        m_engine = nullptr;   // 引擎归通道所有，随通道一起在 I/O 线程销毁
        if (m_csv) {
            m_csv->close();
            if (m_csv->rows() > 0) {
                appendLog(QStringLiteral("CSV 已写入 %1 行：%2").arg(m_csv->rows()).arg(m_csv->path()));
            }
            m_csv.reset();
        }
        delete m_channel;
        m_channel = nullptr;
        // 引擎是异步销毁的，stop() 的 stateChanged 可能来不及送到 UI，
        // 这里直接把状态栏落回"未连接"，避免界面停在"在线"（UI 自动化用例会抓这个）
        m_stateLabel->setText(QStringLiteral("● 未连接"));
        m_connectBtn->setText(QStringLiteral("连接"));
        setConfigEnabled(true);
        return;
    }

    TransportChannel* channel = nullptr;
    if (m_transportBox->currentIndex() == 1) {
        const QString portName = m_serialPortBox->currentText();
        if (portName.isEmpty()) {
            appendLog(QStringLiteral("没有可用串口。现场联调请插设备；本地演示请用 TCP 通道。"));
            return;
        }
        channel = TransportChannel::createSerial(portName, m_baudBox->currentData().toInt(), this);
    } else {
        channel = TransportChannel::createTcp(m_hostEdit->text(), static_cast<quint16>(m_portSpin->value()), this);
    }
    m_channel = channel;
    m_engine = m_channel->createPollEngine();
    m_engine->setSlave(static_cast<uint8_t>(m_slaveSpin->value()));
    m_engine->setRegisterCount(kRegCount);
    m_engine->setIntervalMs(m_intervalSpin->value());

    connect(m_engine, &PollEngine::sample, this, &MainWindow::onSample);
    connect(m_engine, &PollEngine::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_engine, &PollEngine::frameLogged, this, &MainWindow::onFrameLogged);
    connect(m_engine, &PollEngine::writeFinished, this, &MainWindow::onWriteFinished);

    m_scan = new ScanController(m_engine, this);
    connect(m_scan, &ScanController::progress, this, [this](int done, int total, const QString& detail) {
        m_scanBar->setRange(0, total);
        m_scanBar->setValue(done);
        m_scanLabel->setText(detail);
    });
    connect(m_scan, &ScanController::finished, this, [this](const QString& path, int rows) {
        m_scanBtn->setEnabled(true);
        m_scanStopBtn->setEnabled(false);
        m_scanLabel->setText(QStringLiteral("完成 %1 组").arg(rows));
        appendLog(QStringLiteral("测试序列结束：%1 组 → %2").arg(rows).arg(path));
    });
    connect(m_scan, &ScanController::failed, this, [this](const QString& why) {
        appendLog(QStringLiteral("测试序列中断：%1").arg(why));
    });

    m_csv = std::make_unique<CsvLogger>();
    const QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs"));
    if (m_csv->open(dir)) {
        appendLog(QStringLiteral("CSV 落盘：%1").arg(m_csv->path()));
    } else {
        appendLog(QStringLiteral("CSV 打开失败，采集继续但不会落盘：%1").arg(dir));
    }

    m_pointIndex = 0;
    m_tempSeries->clear();
    m_doseSeries->clear();
    m_engine->start();
    m_connectBtn->setText(QStringLiteral("断开"));
    setConfigEnabled(false);
}

void MainWindow::onSample(const QVector<uint16_t>& regs, qint64 timestampMs)
{
    auto valueAt = [&regs](int i) -> uint16_t { return i < regs.size() ? regs.at(i) : 0; };
    m_table->item(0, 1)->setText(QString::number(valueAt(0) / 10.0, 'f', 1));   // 球管温度
    m_table->item(1, 1)->setText(QString::number(valueAt(1)));                  // 实测 kV
    m_table->item(2, 1)->setText(QString::number(valueAt(2)));                  // 实测 mA
    m_table->item(3, 1)->setText(QStringLiteral("0x%1").arg(valueAt(3), 4, 16, QLatin1Char('0')));
    m_table->item(4, 1)->setText(QString::number(valueAt(4) / 100.0, 'f', 2));  // 校准片偏移
    m_table->item(5, 1)->setText(QString::number(valueAt(5) / 10.0, 'f', 1));   // 剂量率
    m_table->item(6, 1)->setText(QString::number(valueAt(6)));                  // 设定 kV
    m_table->item(7, 1)->setText(QString::number(valueAt(7)));                  // 设定 mA

    const double temp = valueAt(0) / 10.0;
    const double dose = valueAt(5) / 10.0;
    m_tempSeries->append(static_cast<qreal>(m_pointIndex), temp);
    m_doseSeries->append(static_cast<qreal>(m_pointIndex), dose);
    if (m_tempSeries->count() > m_chartWindow) {
        m_tempSeries->removePoints(0, m_tempSeries->count() - m_chartWindow);
        m_doseSeries->removePoints(0, m_doseSeries->count() - m_chartWindow);
        for (auto* axis : m_chart->axes(Qt::Horizontal)) {
            if (auto* va = qobject_cast<QValueAxis*>(axis)) {
                va->setRange(m_pointIndex - m_chartWindow + 1, m_pointIndex);
            }
        }
    }
    ++m_pointIndex;

    if (m_csv) {
        const QString stateCode = (m_engine && m_engine->state() == PollEngine::State::Online)
                                      ? QStringLiteral("online")
                                      : QStringLiteral("degraded");
        m_csv->writeRow(timestampMs, regs, stateCode);
    }
    updateCounterLabel();
}

void MainWindow::onStateChanged(PollEngine::State state, const QString& detail)
{
    QString text;
    QString color = QStringLiteral("#555");
    switch (state) {
    case PollEngine::State::Online:
        text = QStringLiteral("在线");
        color = QStringLiteral("#0a7d32");
        break;
    case PollEngine::State::Connecting:
        text = QStringLiteral("连接中");
        color = QStringLiteral("#8a6d00");
        break;
    case PollEngine::State::Backoff:
        text = QStringLiteral("断线重连中");
        color = QStringLiteral("#b45309");
        break;
    case PollEngine::State::Idle:
    case PollEngine::State::Stopped:
        text = QStringLiteral("未连接");
        break;
    }
    m_stateLabel->setText(QStringLiteral("● %1 · %2").arg(text, detail));
    m_stateLabel->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    appendLog(QStringLiteral("[状态] %1 — %2").arg(text, detail));
    updateCounterLabel();
}

void MainWindow::onFrameLogged(const QString& dir, const QByteArray& frame, const QString& note)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString hex = frame.isEmpty() ? QStringLiteral("-") : mb::toHex(frame);
    appendLog(QStringLiteral("%1  %2  %3   %4").arg(ts, dir.leftJustified(3, ' '), hex, note));
}

void MainWindow::onWriteClicked()
{
    if (!m_engine || m_engine->state() != PollEngine::State::Online) {
        appendLog(QStringLiteral("未在线，写请求被忽略"));
        return;
    }
    const auto addr = static_cast<uint16_t>(m_writeAddrSpin->value());
    const auto value = static_cast<uint16_t>(m_writeValueSpin->value());
    if (!m_engine->enqueueWrite(addr, value)) {
        appendLog(QStringLiteral("写请求入队失败"));
        return;
    }
    m_writeBtn->setEnabled(false);
}

void MainWindow::onWriteFinished(bool ok, uint16_t addr, uint16_t value, const QString& detail)
{
    m_writeBtn->setEnabled(true);
    appendLog(ok ? QStringLiteral("写入成功：HR%1 = %2").arg(addr).arg(value)
                 : QStringLiteral("写入失败：HR%1 = %2（%3）").arg(addr).arg(value).arg(detail));
}

void MainWindow::appendLog(const QString& text)
{
    m_log->appendPlainText(text);
}

void MainWindow::updateCounterLabel()
{
    if (!m_counterLabel) {
        return;
    }
    const quint64 ok = m_engine ? m_engine->okFrames() : 0;
    const quint64 to = m_engine ? m_engine->timeouts() : 0;
    const quint64 crc = m_engine ? m_engine->crcErrors() : 0;
    const quint64 rt = m_engine ? m_engine->retries() : 0;
    const int rows = m_csv ? m_csv->rows() : 0;
    m_counterLabel->setText(QStringLiteral("正常帧 %1 ｜ 超时 %2 ｜ CRC 错 %3 ｜ 重试 %4 ｜ CSV %5 行")
                                .arg(ok).arg(to).arg(crc).arg(rt).arg(rows));
}

void MainWindow::setConfigEnabled(bool enabled)
{
    m_transportBox->setEnabled(enabled);
    m_hostEdit->setEnabled(enabled);
    m_portSpin->setEnabled(enabled);
    m_serialPortBox->setEnabled(enabled);
    m_baudBox->setEnabled(enabled);
    m_slaveSpin->setEnabled(enabled);
    m_intervalSpin->setEnabled(enabled);
}
