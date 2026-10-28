#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QTextStream>
#include <QThread>
#include <QTimer>

// 用法：
//   hmi_app                         正常打开界面，手动点「连接」
//   hmi_app --smoke [--port 15020] [--interval 100] [--seconds 6]
//                                   无头冒烟：自动连接 + 定时退出，把结果打到 stdout（给脚本/CI 用）
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("QtHmiModbus"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Modbus RTU 上位机采集监控"));
    parser.addHelpOption();
    const QCommandLineOption smokeOpt(QStringLiteral("smoke"), QStringLiteral("无头冒烟：自动连接并定时退出"));
    const QCommandLineOption hostOpt(QStringLiteral("host"), QStringLiteral("下位机地址"), QStringLiteral("host"),
                                     QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOpt(QStringLiteral("port"), QStringLiteral("下位机端口"), QStringLiteral("port"),
                                     QStringLiteral("15020"));
    const QCommandLineOption intervalOpt(QStringLiteral("interval"), QStringLiteral("采集周期 ms"),
                                         QStringLiteral("ms"), QStringLiteral("100"));
    const QCommandLineOption secondsOpt(QStringLiteral("seconds"), QStringLiteral("运行秒数（仅 --smoke）"),
                                        QStringLiteral("s"), QStringLiteral("6"));
    const QCommandLineOption shotOpt(QStringLiteral("shot"), QStringLiteral("把界面截图存到指定 PNG（仅 --smoke）"),
                                     QStringLiteral("png"));
    const QCommandLineOption scanOpt(QStringLiteral("scan"), QStringLiteral("冒烟时顺带跑一段 kV×mA 测试序列"));
    const QCommandLineOption checkOpt(QStringLiteral("check"), QStringLiteral("离线核对：读入测试数据 CSV 并输出结论"),
                                      QStringLiteral("csv"));
    const QCommandLineOption reportOpt(QStringLiteral("report"), QStringLiteral("核对报告输出路径（Markdown）"),
                                       QStringLiteral("md"));
    const QCommandLineOption uiLoadOpt(QStringLiteral("ui-load"),
                                       QStringLiteral("每 100 ms 在 UI 线程忙等 N ms（模拟重绘/导出压力）"),
                                       QStringLiteral("ms"), QStringLiteral("0"));
    parser.addOptions({smokeOpt, hostOpt, portOpt, intervalOpt, secondsOpt, shotOpt, scanOpt, checkOpt, reportOpt,
                       uiLoadOpt});
    parser.process(app);

    // 离线核对模式：不需要设备，直接对导出的测试数据做判定
    if (parser.isSet(checkOpt)) {
        MainWindow w;   // 不 show()，只用它的核对逻辑
        if (!w.loadQcCsv(parser.value(checkOpt))) {
            QTextStream(stdout) << "check_failed（表头或数据不可用）\n";
            return 2;
        }
        if (parser.isSet(shotOpt)) {   // 需要界面时：切到离线核对页并截图
            const QString shotPath = parser.value(shotOpt);
            w.selectTab(1);
            w.show();
            QTimer::singleShot(1200, &w, [&w, shotPath]() { w.grab().save(shotPath); });
            QTimer::singleShot(1800, &app, &QCoreApplication::quit);
            app.exec();
        }
        if (parser.isSet(reportOpt)) {
            QString err;
            if (!w.exportQcReport(parser.value(reportOpt), &err)) {
                QTextStream(stdout) << "report_failed " << err << "\n";
                return 3;
            }
        }
        QTextStream out(stdout);
        out << "check_result total=" << w.qcTotal() << " passed=" << w.qcPassed()
            << " failed=" << w.qcFailed();
        if (parser.isSet(reportOpt)) {
            out << " report=" << parser.value(reportOpt);
        }
        out << "\n";
        out.flush();
        return 0;
    }

    if (!parser.isSet(smokeOpt)) {
        MainWindow w;
        w.show();
        return app.exec();
    }

    MainWindow w;   // 不 show()：只需事件循环与网络/定时器
    w.prepareAutoConnect(parser.value(hostOpt), static_cast<quint16>(parser.value(portOpt).toUShort()),
                         parser.value(intervalOpt).toInt());
    w.connectNow();
    if (parser.isSet(shotOpt)) {
        const QString shotPath = parser.value(shotOpt);
        w.show();   // 截图需要真实布局尺寸
        QTimer::singleShot(4000, &w, [&w, shotPath]() { w.grab().save(shotPath); });
    }
    if (parser.isSet(scanOpt)) {
        QTimer::singleShot(1500, &w, [&w]() {
            w.startScan({80, 100, 120}, {100, 200, 300});   // 3 × 3 = 9 组
        });
    }
    // 模拟 UI 线程压力：每 100 ms 忙等 N ms，用来验证采集是否被界面拖住
    const int uiLoadMs = parser.value(uiLoadOpt).toInt();
    if (uiLoadMs > 0) {
        auto* busy = new QTimer(&app);
        QObject::connect(busy, &QTimer::timeout, &app, [uiLoadMs]() {
            QElapsedTimer clock;
            clock.start();
            volatile double sink = 0.0;
            while (clock.elapsed() < uiLoadMs) {
                for (int i = 0; i < 20000; ++i) {
                    sink += i * 0.5;
                }
            }
        });
        busy->start(100);
    }
    QTimer::singleShot(parser.value(secondsOpt).toInt() * 1000, &app, &QCoreApplication::quit);

    const int rc = app.exec();

    // 退出前把还在队列里的样本事件排空：采集在 I/O 线程、落盘在 UI 线程，
    // 直接 quit 会丢掉最后几帧（50 ms 周期时尤其明显），CSV 行数就对不上成功帧数了
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    QTextStream out(stdout);
    out << "smoke_result"
        << " ok_frames=" << w.okFrames()
        << " timeouts=" << w.timeouts()
        << " crc_errors=" << w.crcErrors()
        << " retries=" << w.retries()
        << " csv_rows=" << w.csvRows()
        << " scan_rows=" << w.scanRows()
        << " ticks=" << w.engineTicks()
        << " latency_ms=" << QString::number(w.engineLatency(), 'f', 2)
        << " io_thread=" << w.ioThreadId()
        << " ui_thread=" << reinterpret_cast<quintptr>(QThread::currentThread())
        << " csv=" << w.csvPath()
        << " scan=" << w.scanPath() << "\n";
    out.flush();
    return rc;
}
