#!/usr/bin/env python3
"""生成一份"设备测试导出数据"样例（模拟 CT 新机图像质量测试记录）。

用途：给上位机的「离线核对」页做输入，验证 均匀性 / 噪声 判定与报告导出。
数据是**合成的**：不是真实设备测量值，只用来跑通判定逻辑与演示异常标记。

字段：device,batch,kv,ma,ct_center,ct_edge,noise
- ct_center / ct_edge：水模中心 / 边缘 ROI 的 CT 值均值（HU）
- noise：ROI 内标准差（HU）
约定阈值：均匀性 |center-edge| ≤ 8 HU，噪声 ≤ 3.5 HU（可在界面里改）
"""
import csv
import random
from pathlib import Path

random.seed(20261028)

OUT = Path(__file__).resolve().parent.parent / "testdata" / "export_5devices_127.csv"

DEVICES = ["CT-01", "CT-02", "CT-03", "CT-04", "CT-05"]
BATCHES = ["B1", "B2", "B3"]
KV_LIST = [80, 100, 120]
MA_LIST = [100, 150, 200, 250, 300]

# 注入的缺陷：(device, 该设备内的行号) → (均匀性, 噪声)
FAULTS = {
    ("CT-03", 7): (11.8, 2.4),   # 均匀性超限
    ("CT-03", 13): (14.2, 2.6),
    ("CT-03", 19): (9.4, 3.1),
    ("CT-05", 5): (4.1, 4.6),    # 噪声超限
    ("CT-05", 11): (3.2, 5.1),
    ("CT-05", 22): (5.6, 4.2),   # 两项都超
    ("CT-05", 25): (6.8, 3.9),
}


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for dev in DEVICES:
        per_device = 25 if dev != "CT-05" else 27   # 5×25 + 2 = 127
        for i in range(1, per_device + 1):
            kv = KV_LIST[i % len(KV_LIST)]
            ma = MA_LIST[i % len(MA_LIST)]
            batch = BATCHES[i % len(BATCHES)]
            # 正常组：中心 CT 在 0 附近，边缘略偏；剂量越高噪声越低
            center = round(random.gauss(0.0, 1.1), 1)
            uniformity = abs(random.gauss(2.2, 1.0))
            noise = max(0.8, 3.0 - (kv * ma) / 60000.0 + random.gauss(0, 0.25))
            if (dev, i) in FAULTS:
                uniformity, noise = FAULTS[(dev, i)]
            edge = round(center + (uniformity if random.random() < 0.5 else -uniformity), 1)
            rows.append({
                "device": dev,
                "batch": batch,
                "kv": kv,
                "ma": ma,
                "ct_center": f"{center:.1f}",
                "ct_edge": f"{edge:.1f}",
                "noise": f"{noise:.2f}",
            })

    with OUT.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["device", "batch", "kv", "ma", "ct_center", "ct_edge", "noise"])
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {OUT} rows={len(rows)}")
    print(f"injected_faults={len(FAULTS)}")


if __name__ == "__main__":
    main()
