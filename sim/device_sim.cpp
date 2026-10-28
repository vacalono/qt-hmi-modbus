// device_sim —— 模拟 CT 测试样机的参数通道（Modbus RTU over TCP）
//
// 场景：新设备图像质量测试——测试台把 kV / mA 下发到样机，上位机周期读回
// 球管温度、剂量率、校准片偏移与状态位，并按 kV × mA 矩阵成组记录。
// 这台模拟器只复现"寄存器 ↔ 设备参数"这一层，不是任何公司软件的替代品。
//
// 为什么是 RTU over TCP：本机没有 com0com 之类的虚拟串口对，串口通道没法自测；
// Modbus RTU 帧格式与传输无关，塞进 TCP 就能完整验证解析/超时/重连。现场换 QSerialPort 即可。
//
// 寄存器映射（与上位机一致）：
//   HR0 球管温度 ×10 (℃)     HR1 实测 kV        HR2 实测 mA
//   HR3 状态位 0x0001 就绪 / 0x0002 曝光中 / 0x0004 校准中
//   HR4 校准片偏移 ×100 (mm)  HR5 剂量率 ×10 (µGy/s)
//   HR6 设定 kV（可写）       HR7 设定 mA（可写）
//
// 故障注入（环境变量 SIM_FAULT）：
//   drop    —— 每 5 帧丢 1 帧（触发上位机超时与重试）
//   corrupt —— 每 5 帧把 CRC 改坏（触发 CRC 校验失败）
//   slow    —— 每帧延迟 400 ms（触发应答超时）

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtMath>
#include <cstdio>

namespace {

constexpr uint16_t kFuncRead = 0x03;
constexpr uint16_t kFuncWriteSingle = 0x06;
constexpr int kRegCount = 8;
constexpr int kRegSetKv = 6;
constexpr int kRegSetMa = 7;
constexpr quint16 kPort = 15020;

uint16_t crc16(const QByteArray& data)
{
    uint16_t crc = 0xFFFF;
    for (char ch : data) {
        crc ^= static_cast<uint8_t>(ch);
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

QByteArray withCrc(QByteArray frame)
{
    const uint16_t crc = crc16(frame);
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
    return frame;
}

class DeviceSim : public QObject
{
public:
    explicit DeviceSim(QObject* parent = nullptr) : QObject(parent)
    {
        m_fault = qEnvironmentVariable("SIM_FAULT");
        m_regs[0] = 300;    // 30.0 ℃
        m_regs[1] = 100;    // 100 kV
        m_regs[2] = 200;    // 200 mA
        m_regs[3] = 0x0001; // 就绪
        m_regs[4] = 0;      // 校准片偏移 0.00 mm
        m_regs[5] = 1200;   // 剂量率 120.0 µGy/s
        m_regs[6] = 100;    // 设定 kV
        m_regs[7] = 200;    // 设定 mA

        connect(&m_server, &QTcpServer::newConnection, this, &DeviceSim::onNewConnection);
        m_tick.start(100);
        connect(&m_tick, &QTimer::timeout, this, [this]() { m_elapsed += 0.1; });
    }

    bool listen()
    {
        if (!m_server.listen(QHostAddress::LocalHost, kPort)) {
            qCritical() << "listen failed:" << m_server.errorString();
            return false;
        }
        qInfo().noquote() << QStringLiteral("CT 测试样机模拟器已启动：127.0.0.1:%1（RTU over TCP，从站 1）").arg(kPort);
        qInfo().noquote() << QStringLiteral("寄存器：HR0 球管温度 / HR1 实测kV / HR2 实测mA / HR3 状态 / HR4 校准偏移 / HR5 剂量率 / HR6 设定kV / HR7 设定mA");
        qInfo().noquote() << QStringLiteral("故障注入：%1").arg(m_fault.isEmpty() ? QStringLiteral("无") : m_fault);
        return true;
    }

private:
    void onNewConnection()
    {
        while (QTcpSocket* sock = m_server.nextPendingConnection()) {
            m_buffers.insert(sock, QByteArray());
            qInfo().noquote() << "client connected:" << sock->peerAddress().toString() << sock->peerPort();
            connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { onReadyRead(sock); });
            connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
                m_buffers.remove(sock);
                sock->deleteLater();
                qInfo().noquote() << "client disconnected";
            });
        }
    }

    void onReadyRead(QTcpSocket* sock)
    {
        QByteArray& buf = m_buffers[sock];
        buf.append(sock->readAll());
        while (true) {
            if (buf.size() < 8) {   // 0x03 / 0x06 请求都是 8 字节
                return;
            }
            const uint8_t func = static_cast<uint8_t>(buf.at(1));
            int frameLen = 8;
            if (func == 0x10) {     // 0x10 写多个：读 byteCount
                const int byteCount = static_cast<uint8_t>(buf.at(6));
                frameLen = 7 + byteCount + 2;
                if (buf.size() < frameLen) {
                    return;
                }
            }
            const QByteArray frame = buf.left(frameLen);
            buf.remove(0, frameLen);
            handleFrame(sock, frame);
        }
    }

    QByteArray buildReadResponse(uint8_t slave, uint16_t addr, uint16_t count)
    {
        QByteArray resp;
        resp.append(static_cast<char>(slave));
        resp.append(static_cast<char>(kFuncRead));
        resp.append(static_cast<char>(count * 2));
        for (uint16_t i = 0; i < count; ++i) {
            const uint16_t v = (addr + i) < kRegCount ? m_regs[addr + i] : 0;
            resp.append(static_cast<char>((v >> 8) & 0xFF));
            resp.append(static_cast<char>(v & 0xFF));
        }
        return withCrc(resp);
    }

    void handleFrame(QTcpSocket* sock, const QByteArray& frame)
    {
        ++m_frameNo;
        const uint16_t crcRecv = static_cast<uint8_t>(frame.at(frame.size() - 2))
                                 | (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(frame.size() - 1))) << 8);
        const bool crcOk = crc16(frame.left(frame.size() - 2)) == crcRecv;
        qInfo().noquote() << QStringLiteral("RX[%1] %2  crc=%3")
                                 .arg(m_frameNo)
                                 .arg(QString::fromLatin1(frame.toHex(' ')).toUpper())
                                 .arg(crcOk ? QStringLiteral("ok") : QStringLiteral("BAD"));
        if (!crcOk) {
            return;   // CRC 错的帧直接丢，符合从站行为
        }

        const uint8_t slave = static_cast<uint8_t>(frame.at(0));
        const uint8_t func = static_cast<uint8_t>(frame.at(1));
        if (slave != 1) {
            return;   // 本模拟器只做从站 1
        }

        updateRegisters();

        QByteArray resp;
        if (func == kFuncRead) {
            const uint16_t addr = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(2))) << 8)
                                  | static_cast<uint8_t>(frame.at(3));
            const uint16_t count = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(4))) << 8)
                                   | static_cast<uint8_t>(frame.at(5));
            resp = buildReadResponse(slave, addr, count);
        } else if (func == kFuncWriteSingle) {
            const uint16_t addr = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(2))) << 8)
                                  | static_cast<uint8_t>(frame.at(3));
            const uint16_t value = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(4))) << 8)
                                   | static_cast<uint8_t>(frame.at(5));
            if (addr >= kRegCount || addr < kRegSetKv) {   // 只允许写设定值 HR6/HR7
                QByteArray ex;
                ex.append(static_cast<char>(slave));
                ex.append(static_cast<char>(func | 0x80));
                ex.append(static_cast<char>(0x02));   // 非法数据地址
                resp = withCrc(ex);
            } else {
                m_regs[addr] = value;
                qInfo().noquote() << QStringLiteral("  写 HR%1 = %2").arg(addr).arg(value);
                resp = frame.left(6);                 // 0x06 的正常响应＝请求原样回显
                resp = withCrc(resp);
            }
        } else {
            QByteArray ex;
            ex.append(static_cast<char>(slave));
            ex.append(static_cast<char>(func | 0x80));
            ex.append(static_cast<char>(0x01));       // 非法功能码
            resp = withCrc(ex);
        }

        if (!m_fault.isEmpty() && (m_frameNo % 5 == 0)) {
            if (m_fault == QStringLiteral("drop")) {
                qInfo().noquote() << "  [fault] drop response";
                return;
            }
            if (m_fault == QStringLiteral("corrupt")) {
                resp[resp.size() - 1] = static_cast<char>(resp.at(resp.size() - 1) ^ 0xFF);
                qInfo().noquote() << "  [fault] corrupt CRC";
            }
            if (m_fault == QStringLiteral("slow")) {
                QTimer::singleShot(400, sock, [sock, resp]() { sock->write(resp); });
                qInfo().noquote() << "  [fault] delay 400ms";
                return;
            }
        }

        sock->write(resp);
        qInfo().noquote() << QStringLiteral("TX      %1").arg(QString::fromLatin1(resp.toHex(' ')).toUpper());
    }

    // 读数跟着设定值走，并带物理惯性：kV/mA 上去后球管温度按一阶惯性爬升
    void updateRegisters()
    {
        const double t = m_elapsed;
        const int setKv = m_regs[kRegSetKv];
        const int setMa = m_regs[kRegSetMa];

        m_regs[1] = static_cast<uint16_t>(setKv + 1.5 * qSin(t / 2.0));   // 实测 kV 有 ±1.5 抖动
        m_regs[2] = static_cast<uint16_t>(setMa + 8.0 * qSin(t / 3.0));   // 实测 mA 有 ±8 抖动

        const double heatTarget = 28.0 + 0.15 * setKv + 0.06 * setMa;
        m_heat += (heatTarget - m_heat) * 0.02;
        m_regs[0] = static_cast<uint16_t>(m_heat * 10.0);

        m_regs[3] = m_exposing ? 0x0002 : 0x0001;                        // 曝光中 / 就绪
        m_regs[4] = static_cast<uint16_t>(qAbs(qSin(t / 11.0)) * 12.0);   // 校准片偏移 0~0.12 mm
        m_regs[5] = static_cast<uint16_t>(setKv * setMa / 40.0 * 10.0);   // 剂量率与功率近似成正比
        m_exposing = !m_exposing;
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QTimer m_tick;
    double m_elapsed = 0.0;
    double m_heat = 30.0;
    bool m_exposing = false;
    uint16_t m_regs[kRegCount] = {0};
    QString m_fault;
    int m_frameNo = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // 日志同时写 stderr 和文件（stderr 重定向时会被缓冲，文件便于事后核对）
    static QFile logFile;
    logFile.setFileName(QStringLiteral("device_sim.log"));
    logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& msg) {
        const QByteArray line = msg.toUtf8() + "\n";
        fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
        fflush(stderr);
        if (logFile.isOpen()) {
            logFile.write(line);
            logFile.flush();
        }
    });

    DeviceSim sim;
    if (!sim.listen()) {
        return 1;
    }
    return app.exec();
}
