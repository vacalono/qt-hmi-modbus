#include "protocol/ModbusFrame.h"

namespace mb {

uint16_t crc16(const uint8_t* data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x0001) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

uint16_t crc16(const QByteArray& data)
{
    return crc16(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
}

static void appendCrc(QByteArray& frame)
{
    const uint16_t crc = crc16(frame);
    frame.append(static_cast<char>(crc & 0xFF));        // 低字节在前
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
}

static void appendU16(QByteArray& frame, uint16_t v)
{
    frame.append(static_cast<char>((v >> 8) & 0xFF));   // 寄存器值高字节在前
    frame.append(static_cast<char>(v & 0xFF));
}

QByteArray buildReadHolding(uint8_t slave, uint16_t addr, uint16_t count)
{
    QByteArray f;
    f.append(static_cast<char>(slave));
    f.append(static_cast<char>(kFuncReadHolding));
    appendU16(f, addr);
    appendU16(f, count);
    appendCrc(f);
    return f;
}

QByteArray buildWriteSingle(uint8_t slave, uint16_t addr, uint16_t value)
{
    QByteArray f;
    f.append(static_cast<char>(slave));
    f.append(static_cast<char>(kFuncWriteSingle));
    appendU16(f, addr);
    appendU16(f, value);
    appendCrc(f);
    return f;
}

QByteArray buildWriteMultiple(uint8_t slave, uint16_t addr, const QVector<uint16_t>& values)
{
    QByteArray f;
    f.append(static_cast<char>(slave));
    f.append(static_cast<char>(kFuncWriteMultiple));
    appendU16(f, addr);
    appendU16(f, static_cast<uint16_t>(values.size()));
    f.append(static_cast<char>(values.size() * 2));
    for (uint16_t v : values) {
        appendU16(f, v);
    }
    appendCrc(f);
    return f;
}

int expectedReadResponseSize(uint16_t count)
{
    // slave + func + byteCount + data + crc(2)
    return 1 + 1 + 1 + static_cast<int>(count) * 2 + 2;
}

QString toHex(const QByteArray& data)
{
    return QString::fromLatin1(data.toHex(' ')).toUpper();
}

Response parseReadHolding(const QByteArray& frame, uint8_t slave, uint16_t expectedCount)
{
    Response r;
    if (frame.size() < 5) {
        r.error = QStringLiteral("帧长不足 5 字节");
        return r;
    }
    const uint16_t crcRecv = static_cast<uint8_t>(frame.at(frame.size() - 2))
                             | (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(frame.size() - 1))) << 8);
    if (crc16(frame.left(frame.size() - 2)) != crcRecv) {
        r.error = QStringLiteral("CRC 校验失败");
        return r;
    }
    r.slave = static_cast<uint8_t>(frame.at(0));
    r.func  = static_cast<uint8_t>(frame.at(1));
    if (r.slave != slave) {
        r.error = QStringLiteral("从站地址不匹配");
        return r;
    }
    if (r.func == (kFuncReadHolding | 0x80)) {
        r.exceptionCode = static_cast<uint8_t>(frame.at(2));
        r.error = QStringLiteral("从站返回异常码 0x%1").arg(r.exceptionCode, 2, 16, QLatin1Char('0'));
        return r;
    }
    if (r.func != kFuncReadHolding) {
        r.error = QStringLiteral("功能码不匹配");
        return r;
    }
    const int byteCount = static_cast<uint8_t>(frame.at(2));
    if (byteCount != expectedCount * 2 || frame.size() != 3 + byteCount + 2) {
        r.error = QStringLiteral("数据长度与请求不一致");
        return r;
    }
    for (int i = 0; i < byteCount; i += 2) {
        const uint16_t v = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(3 + i))) << 8)
                           | static_cast<uint8_t>(frame.at(4 + i));
        r.registers.append(v);
    }
    r.ok = true;
    return r;
}

Response parseWriteSingle(const QByteArray& frame, uint8_t slave, uint16_t addr, uint16_t value)
{
    Response r;
    if (frame.size() != 8) {
        r.error = QStringLiteral("写单寄存器响应应为 8 字节");
        return r;
    }
    const uint16_t crcRecv = static_cast<uint8_t>(frame.at(6))
                             | (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(7))) << 8);
    if (crc16(frame.left(6)) != crcRecv) {
        r.error = QStringLiteral("CRC 校验失败");
        return r;
    }
    r.slave = static_cast<uint8_t>(frame.at(0));
    r.func  = static_cast<uint8_t>(frame.at(1));
    if (r.func == (kFuncWriteSingle | 0x80)) {
        r.exceptionCode = static_cast<uint8_t>(frame.at(2));
        r.error = QStringLiteral("从站返回异常码 0x%1").arg(r.exceptionCode, 2, 16, QLatin1Char('0'));
        return r;
    }
    const uint16_t rAddr = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(2))) << 8)
                           | static_cast<uint8_t>(frame.at(3));
    const uint16_t rVal  = (static_cast<uint16_t>(static_cast<uint8_t>(frame.at(4))) << 8)
                           | static_cast<uint8_t>(frame.at(5));
    if (r.slave != slave || r.func != kFuncWriteSingle || rAddr != addr || rVal != value) {
        r.error = QStringLiteral("回显与请求不一致");
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace mb
