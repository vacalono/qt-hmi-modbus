#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include "protocol/ModbusFrame.h"
#include "transport/TransportChannel.h"

namespace {

// 测试里手工拼一个响应帧：从站 + 功能码 + 数据 + CRC
QByteArray frameWithCrc(QByteArray body)
{
    const uint16_t crc = mb::crc16(body);
    body.append(static_cast<char>(crc & 0xFF));
    body.append(static_cast<char>((crc >> 8) & 0xFF));
    return body;
}

} // namespace

class TestModbus : public QObject
{
    Q_OBJECT

private slots:
    // CRC-16/MODBUS 的标准校验值：字符串 "123456789" → 0x4B37
    void crcKnownCheckValue()
    {
        QCOMPARE(mb::crc16(QByteArray("123456789")), static_cast<uint16_t>(0x4B37));
    }

    void readRequestLayout()
    {
        const QByteArray f = mb::buildReadHolding(0x01, 0x0000, 5);
        QCOMPARE(f.size(), 8);
        QCOMPARE(static_cast<uint8_t>(f.at(0)), uint8_t(0x01));
        QCOMPARE(static_cast<uint8_t>(f.at(1)), mb::kFuncReadHolding);
        QCOMPARE(static_cast<uint8_t>(f.at(5)), uint8_t(5));          // 寄存器数量低字节
        // 末两字节必须是前 6 字节的 CRC，且低字节在前
        const uint16_t crc = mb::crc16(f.left(6));
        QCOMPARE(static_cast<uint8_t>(f.at(6)), static_cast<uint8_t>(crc & 0xFF));
        QCOMPARE(static_cast<uint8_t>(f.at(7)), static_cast<uint8_t>((crc >> 8) & 0xFF));
    }

    void parseReadResponse()
    {
        QByteArray body;
        body.append(char(0x01));       // slave
        body.append(char(0x03));       // func
        body.append(char(6));          // byte count = 3 寄存器
        body.append(char(0x00)); body.append(char(0xFA));   // 250
        body.append(char(0x27)); body.append(char(0x74));   // 10100
        body.append(char(0x05)); body.append(char(0xDC));   // 1500
        const QByteArray frame = frameWithCrc(body);

        const mb::Response r = mb::parseReadHolding(frame, 0x01, 3);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.registers.size(), 3);
        QCOMPARE(r.registers.at(0), static_cast<uint16_t>(250));
        QCOMPARE(r.registers.at(1), static_cast<uint16_t>(10100));
        QCOMPARE(r.registers.at(2), static_cast<uint16_t>(1500));
    }

    void parseExceptionResponse()
    {
        QByteArray body;
        body.append(char(0x01));
        body.append(char(mb::kFuncReadHolding | 0x80));   // 异常响应
        body.append(char(0x02));                          // 非法数据地址
        const QByteArray frame = frameWithCrc(body);

        const mb::Response r = mb::parseReadHolding(frame, 0x01, 3);
        QVERIFY(!r.ok);
        QCOMPARE(r.exceptionCode, uint8_t(0x02));
    }

    void rejectBadCrc()
    {
        QByteArray body;
        body.append(char(0x01));
        body.append(char(0x03));
        body.append(char(2));
        body.append(char(0x00)); body.append(char(0x0A));
        QByteArray frame = frameWithCrc(body);
        frame[frame.size() - 1] = static_cast<char>(frame.at(frame.size() - 1) ^ 0xFF);   // 破坏 CRC

        const mb::Response r = mb::parseReadHolding(frame, 0x01, 1);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("CRC")));
    }

    void parseWriteEcho()
    {
        const QByteArray frame = mb::buildWriteSingle(0x01, 0x0004, 1200);
        const mb::Response r = mb::parseWriteSingle(frame, 0x01, 0x0004, 1200);
        QVERIFY2(r.ok, qPrintable(r.error));

        // 回显值不一致必须判失败（防止"写了但设备没接受"被当成成功）
        const mb::Response bad = mb::parseWriteSingle(frame, 0x01, 0x0004, 1300);
        QVERIFY(!bad.ok);
    }

    void responseSizeHelper()
    {
        QCOMPARE(mb::expectedReadResponseSize(5), 5 + 2 * 5);   // 1+1+1+2*5+2 = 15
    }

    // ── 传输线程回归：I/O 必须在独立线程，且字节能跨线程送回 UI ──
    void ioThreadIsSeparate()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        TransportChannel* channel = TransportChannel::createTcp(QStringLiteral("127.0.0.1"), server.serverPort());
        QVERIFY(channel->open());
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(channel->ioThreadId() != 0, 3000);

        QVERIFY2(channel->ioThreadId() != reinterpret_cast<quintptr>(QThread::currentThread()),
                 "I/O 线程与 UI 线程相同，说明 worker 没有跑在独立线程里");

        delete server.nextPendingConnection();
        delete channel;
    }

    void bytesCrossThreads()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        TransportChannel* channel = TransportChannel::createTcp(QStringLiteral("127.0.0.1"), server.serverPort());
        QSignalSpy recv(channel, &TransportChannel::bytesReceived);
        QVERIFY(channel->open());
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        QTcpSocket* peer = server.nextPendingConnection();
        QVERIFY(peer);

        // 下位机 → 上位机：worker 线程收字节，队列信号送到主线程
        peer->write("HELLO");
        peer->flush();
        QTRY_COMPARE_WITH_TIMEOUT(recv.count(), 1, 3000);
        QCOMPARE(recv.at(0).at(0).toByteArray(), QByteArray("HELLO"));

        // 上位机 → 下位机：主线程调用 write，worker 线程真正发出去
        QVERIFY(channel->write(QByteArray("PING")));
        QTRY_VERIFY_WITH_TIMEOUT(peer->bytesAvailable() >= 4, 3000);
        QCOMPARE(peer->readAll(), QByteArray("PING"));

        delete peer;
        delete channel;
    }
};

QTEST_MAIN(TestModbus)
#include "test_modbus.moc"
