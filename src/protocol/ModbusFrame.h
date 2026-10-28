#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

// Modbus RTU 帧编解码（从站地址 + 功能码 + 数据 + CRC16）。
// 只实现上位机侧够用的三个功能码：0x03 读保持寄存器 / 0x06 写单个 / 0x10 写多个。
// 设计说明：CRC 与帧格式在这里集中处理，传输层（串口 / TCP）只负责搬字节，
// 这样同一套编解码既能在串口上跑，也能在"RTU over TCP"上跑，便于本地联调。
namespace mb {

constexpr uint8_t kFuncReadHolding  = 0x03;
constexpr uint8_t kFuncWriteSingle  = 0x06;
constexpr uint8_t kFuncWriteMultiple = 0x10;

// CRC-16/MODBUS（多项式 0xA001，初值 0xFFFF，低字节在前）
uint16_t crc16(const uint8_t* data, int len);
uint16_t crc16(const QByteArray& data);

QByteArray buildReadHolding(uint8_t slave, uint16_t addr, uint16_t count);
QByteArray buildWriteSingle(uint8_t slave, uint16_t addr, uint16_t value);
QByteArray buildWriteMultiple(uint8_t slave, uint16_t addr, const QVector<uint16_t>& values);

struct Response {
    bool ok = false;
    uint8_t slave = 0;
    uint8_t func = 0;
    uint8_t exceptionCode = 0;        // 0 表示正常响应
    QVector<uint16_t> registers;      // 仅 0x03 有效
    QString error;                    // 失败原因（给人看）
};

// 期望长度不足、CRC 错、从站/功能码不匹配、异常响应都会体现在 ok/error/exceptionCode 上，
// 调用方不需要自己判断帧长。
Response parseReadHolding(const QByteArray& frame, uint8_t slave, uint16_t expectedCount);
Response parseWriteSingle(const QByteArray& frame, uint8_t slave, uint16_t addr, uint16_t value);

// 一次读响应需要的字节数（用于粘包/半包时判断"够不够一个完整帧"）
int expectedReadResponseSize(uint16_t count);

QString toHex(const QByteArray& data);

} // namespace mb
