# 测试策略（分层 + 每层的断言）

> 这个项目是我一个人写的，所以测试也按"能自动拦住回归"来设计，不是补几张截图。

## L1 · 单元测试（QtTest，`tests/test_modbus.cpp`）

跑：`cd out/tests && qmake ../../tests/tests.pro CONFIG+=release && make && ./release/tst_modbus`

| 用例 | 断言什么 |
|---|---|
| `crcKnownCheckValue` | CRC-16/MODBUS 标准校验值（"123456789" → 0x4B37） |
| `readRequestLayout` | 请求帧布局、CRC 位置 |
| `parseReadResponse` | 正常响应解析 |
| `parseExceptionResponse` | 异常响应（功能码 \| 0x80）要抛异常码而不是当数据 |
| `rejectBadCrc` | CRC 错必须拒绝并给出原因 |
| `parseWriteEcho` | 写回显值不一致要判失败（防"写了没生效"被当成功） |
| `responseSizeHelper` | 期望帧长计算（1+1+1+2N+2） |
| `ioThreadIsSeparate` | I/O 线程必须 ≠ UI 线程 |
| `bytesCrossThreads` | 字节双向跨线程不丢（下位机→上位机、上位机→下位机） |

现状：**11 passed**（9 个用例 + init/cleanup）。

## L2 · 端到端接口自动化（pytest，`tests/e2e/test_hmi_cli.py`）

把上位机当黑盒，只通过 CLI 与导出文件断言——不碰内部类，换成别的实现这套用例照样有效。

跑：

```bash
pip install pytest
HMI_APP=build/app/hmi_app SIM_APP=build/sim/device_sim pytest tests/e2e -v
# 本机 Windows 也可直接 pytest（自动找 dist/ 或 out/app/release/）
```

| 用例 | 断言（失败即回归） |
|---|---|
| `test_sampling_rate_follows_interval[50/100/300]` | 帧数 ≈ 时长/周期（75%–105% 区间），超时 0、CRC 0、CSV 行数 = 成功帧数 |
| `test_first_frame_within_two_seconds` | 第一帧 ≤ 2 s（历史 bug：定时器建错线程 → 3.5 s 才出帧） |
| `test_ui_load_does_not_starve_sampling` | UI 线程忙等 200 ms/100 ms，帧数仍 ≥ 70% 理论值 |
| `test_scan_matrix_records_all_groups` | 3×3 矩阵必须落盘 9 组 |
| `test_dropped_frames_become_timeouts_not_lost` | 丢帧 → 超时 > 0、重试 = 超时、成功+超时 ≤ 节拍（无重复计数）、采集不中断 |
| `test_corrupted_frames_counted_as_crc_errors` | 坏 CRC → CRC 错 > 0，且不进 CSV |
| `test_offline_qc_matches_injected_defects` | 127 组 → 120/7，且明细分布 CT-05=4、CT-03=3（对得上注入的缺陷） |
| `test_offline_qc_rejects_missing_input` | 输入不存在 → 非零退出 + 可读错误，不能崩 |

现状：**10 passed**（Windows 本机 57 s；Linux CI 同样跑）。

## L2b · UI 交互自动化（QtTest，`tests/ui/test_ui.cpp`）

真的点控件，不调私有方法：`QTest::mouseClick` 点「连接」→ 等状态栏变「在线」→ 点页签来回切 → 点「开始序列」等 3×3 全部落盘 → 点「断开」等状态栏回「未连接」。

- 关键控件加了 `objectName`（`connectButton` / `scanButton` / `mainTabs` / `stateLabel` / `intervalSpin`），测试用 `findChild` 拿；
- 用例自带模拟样机（`SIM_APP` 或自动找 `dist/device_sim`），跑完必回收；
- `QT_QPA_PLATFORM=offscreen` 下可跑（CI 用这种方式）。

这条用例第一次跑就抓到一个真问题：**断开后状态栏停在「在线」**（引擎在 I/O 线程里异步销毁，`stop()` 的 `stateChanged` 来不及送到 UI），修法是断开分支里显式把状态栏落回「未连接」。

## L2c · 长跑稳定性（soak，`tests/e2e/test_soak.py`）

```bash
SOAK_SECONDS=1800 pytest tests/e2e/test_soak.py -m soak -v     # 30 分钟
```

默认 `addopts = -m "not soak"`，所以普通 `pytest` 不会跑它；CI 里单独跑一个短版本。

断言：帧数 ≥ 理论值 90%、超时 0、CRC 0、CSV 行数 = 成功帧数、**工作集漂移**（去掉预热段）< 32 MB 且 < 25%。内存读的是真进程工作集（Windows `GetProcessMemoryInfo`，Linux `/proc/<pid>/status`），不是拍脑袋估算。

## L3 · CI 门（GitHub Actions）

每次 push 顺序跑：三个工程构建 → QtTest → 8 s 冒烟（帧 ≥ 70、0 超时、0 CRC）→ UI 压力回归（帧 ≥ 70 且 io_thread ≠ ui_thread）→ pytest 端到端 → 127 组离线核对 → xvfb 桌面截图。任一红即失败，产物（报告 + 截图 + junit）上传。

## 覆盖不到的地方（面试要主动说）

1. **没有真机**：串口通道用 QSerialPort 实现，但没接过真实下位机；TCP 走的是本地 loopback。
2. **UI 交互**没有自动化（Qt 的 QTest::mouseClick 可以补，本项目只做了 --ui-load 的压力模拟）。
3. **没有长跑**：最长 22 s（扫描序列），没有 8 小时稳定性 / 内存增长监控。
4. **性能只有量级指标**（帧数、超时数），没做 CPU/内存 profile。
