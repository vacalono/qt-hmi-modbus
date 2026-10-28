// UI 交互自动化：真的点按钮、切页签，验证状态迁移与界面契约。
// 依赖模拟样机（SIM_APP 或 dist/device_sim），跑在 offscreen 平台上也能工作。

#include <QtTest/QtTest>

#include <QProcess>
#include <QPushButton>
#include <QTcpSocket>
#include <QTabBar>
#include <QTabWidget>
#include <QLabel>

#include "ui/MainWindow.h"

namespace {

QString findSimulator()
{
    const QString env = qEnvironmentVariable("SIM_APP");
    if (!env.isEmpty() && QFileInfo::exists(env)) {
        return env;
    }
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(dir).filePath("device_sim.exe"),
        QDir(dir).filePath("device_sim"),
        QDir(dir).filePath("../../dist/device_sim.exe"),
        QDir(dir).filePath("../../../dist/device_sim.exe"),
        QDir(dir).filePath("../../dist/device_sim"),
        QDir(dir).filePath("../../../dist/device_sim"),
    };
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c)) {
            return QDir::cleanPath(c);
        }
    }
    return QString();
}

bool portOpen(quint16 port)
{
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    const bool ok = s.waitForConnected(300);
    s.abort();
    return ok;
}

} // namespace

class TestUi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QString sim = findSimulator();
        if (sim.isEmpty()) {
            QSKIP("找不到 device_sim，跳过 UI 交互用例（用 SIM_APP 指定路径）");
        }
        m_sim.setProcessChannelMode(QProcess::MergedChannels);
        m_sim.start(sim, {});
        QVERIFY2(m_sim.waitForStarted(5000), "模拟样机没起来");

        // 等端口可连：最多 10 s
        bool ready = false;
        for (int i = 0; i < 100 && !ready; ++i) {
            ready = portOpen(15020);
            if (!ready) {
                QTest::qWait(100);
            }
        }
        QVERIFY2(ready, "模拟样机没有监听 15020");
    }

    void cleanupTestCase()
    {
        if (m_sim.state() != QProcess::NotRunning) {
            m_sim.terminate();
            if (!m_sim.waitForFinished(5000)) {
                m_sim.kill();
            }
        }
    }

    // 一条完整交互链：连接 → 状态变在线 → 切页签 → 跑序列 → 断开
    void connectScanDisconnect()
    {
        MainWindow w;
        w.resize(1180, 760);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto* connectBtn = w.findChild<QPushButton*>(QStringLiteral("connectButton"));
        auto* stateLabel = w.findChild<QLabel*>(QStringLiteral("stateLabel"));
        auto* tabs = w.findChild<QTabWidget*>(QStringLiteral("mainTabs"));
        auto* scanBtn = w.findChild<QPushButton*>(QStringLiteral("scanButton"));
        QVERIFY2(connectBtn && stateLabel && tabs && scanBtn, "关键控件缺少 objectName，UI 自动化拿不到");

        QCOMPARE(connectBtn->text(), QStringLiteral("连接"));
        QVERIFY(stateLabel->text().contains(QStringLiteral("未连接")));

        // ① 点「连接」
        QTest::mouseClick(connectBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(stateLabel->text().contains(QStringLiteral("在线")), 10000);
        QCOMPARE(connectBtn->text(), QStringLiteral("断开"));

        // ② 点页签：切到「离线核对」再切回「在线采集」
        QTabBar* bar = tabs->tabBar();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(1).center());
        QCOMPARE(tabs->currentIndex(), 1);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
        QCOMPARE(tabs->currentIndex(), 0);

        // ③ 点「开始序列」，等 3×3 全部落盘
        QTest::mouseClick(scanBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.scanRows() == 9, 45000);

        // ④ 点「断开」，状态回落
        QTest::mouseClick(connectBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(connectBtn->text() == QStringLiteral("连接"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(stateLabel->text().contains(QStringLiteral("未连接")), 5000);
    }

private:
    QProcess m_sim;
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
