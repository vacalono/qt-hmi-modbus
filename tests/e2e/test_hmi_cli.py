"""端到端接口自动化：把上位机当黑盒，用命令行接口（CLI）做断言。

分工：
- QtTest（tests/test_modbus.cpp）测协议与线程契约，属白盒单元测试；
- 这里测「可观测行为」：帧数、超时、CRC 错、CSV 行数、核对总数、报告内容；
- CI 里两者都跑，任一红即失败。

二进制路径可用环境变量覆盖（CI 里 build/，本机 dist/ 或 out/app/release/）。
"""

from __future__ import annotations

import csv
import os
import re
import socket
import subprocess
import time
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HOST = "127.0.0.1"
PORT = 15020


def _first_existing(*candidates: Path) -> Path | None:
    for c in candidates:
        if c.is_file():
            return c
    return None


def _find_app() -> Path | None:
    env = os.environ.get("HMI_APP")
    if env:
        return Path(env)
    return _first_existing(
        ROOT / "dist" / "hmi_app.exe",
        ROOT / "dist" / "hmi_app",
        ROOT / "out" / "app" / "release" / "hmi_app.exe",
        ROOT / "build" / "app" / "hmi_app",
    )


def _find_sim() -> Path | None:
    env = os.environ.get("SIM_APP")
    if env:
        return Path(env)
    return _first_existing(
        ROOT / "dist" / "device_sim.exe",
        ROOT / "dist" / "device_sim",
        ROOT / "out" / "sim" / "release" / "device_sim.exe",
        ROOT / "build" / "sim" / "device_sim",
    )


APP = _find_app()
SIM = _find_sim()


def pytest_configure(config):
    config.addinivalue_line("markers", "e2e: 需要上位机与模拟下位机可执行文件")


def _require_binaries():
    if not APP or not APP.exists():
        pytest.skip(f"找不到上位机可执行文件（可用 HMI_APP 指定），当前 {APP}")
    if not SIM or not SIM.exists():
        pytest.skip(f"找不到模拟下位机（可用 SIM_APP 指定），当前 {SIM}")


def _env(fault: str | None = None) -> dict:
    env = os.environ.copy()
    env.pop("SIM_FAULT", None)
    if fault:
        env["SIM_FAULT"] = fault
    # Linux CI 没有显示器：走 offscreen；Windows 上不设，正常开窗
    if os.name != "nt":
        env.setdefault("QT_QPA_PLATFORM", "offscreen")
    return env


def _wait_port(port: int, timeout: float = 10.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.3):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError(f"模拟下位机没有在 {timeout}s 内监听 {HOST}:{port}")


@contextmanager
def simulator(fault: str | None = None):
    """起一个模拟样机；退出时一定回收，避免端口占用污染下一个用例。"""
    _require_binaries()
    proc = subprocess.Popen(
        [str(SIM)],
        cwd=str(ROOT),
        env=_env(fault),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        _wait_port(PORT)
        yield proc
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def run_app(*args: str, timeout: float = 180.0) -> tuple[str, float]:
    """跑一次上位机，返回 (stdout, 耗时秒)。"""
    _require_binaries()
    t0 = time.time()
    out = subprocess.run(
        [str(APP), *args],
        cwd=str(ROOT),
        env=_env(),
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    elapsed = time.time() - t0
    assert out.returncode == 0, f"退出码 {out.returncode}\nstdout={out.stdout}\nstderr={out.stderr}"
    return out.stdout, elapsed


RESULT_RE = re.compile(r"^(smoke_result|check_result)\s+(.*)$", re.MULTILINE)


def parse_result(stdout: str) -> dict[str, str]:
    """把 `smoke_result ok_frames=79 timeouts=0 ...` 解析成 dict。"""
    m = RESULT_RE.search(stdout)
    assert m, f"输出里没有 *_result 行：\n{stdout}"
    body = m.group(2)
    result: dict[str, str] = {}
    for token in body.split():
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        result[k] = v
    result["_kind"] = m.group(1)
    return result


def num(result: dict[str, str], key: str) -> int:
    assert key in result, f"结果里缺 {key}：{result}"
    return int(result[key])


def csv_rows(path: str | Path) -> int:
    p = Path(path)
    assert p.is_file(), f"CSV 不存在：{p}"
    with open(p, encoding="utf-8", newline="") as f:
        return max(0, sum(1 for _ in f) - 1)   # 去掉表头


# ── 采集类 ────────────────────────────────────────────────────────────


@pytest.mark.e2e
@pytest.mark.parametrize("interval", [50, 100, 300])
def test_sampling_rate_follows_interval(interval: int):
    """采样节拍必须跟着 --interval 走：帧数 ≈ 时长/周期，且零错误。"""
    seconds = 4
    with simulator():
        stdout, _ = run_app("--smoke", "--seconds", str(seconds), "--interval", str(interval))
    r = parse_result(stdout)
    frames = num(r, "ok_frames")
    expected = seconds * 1000 / interval
    assert frames >= expected * 0.75, f"interval={interval} 只采到 {frames} 帧（期望 ≈{expected:.0f}）"
    assert frames <= expected * 1.05, f"interval={interval} 采到 {frames} 帧，超过理论值（重复计数？）"
    assert num(r, "timeouts") == 0
    assert num(r, "crc_errors") == 0
    assert csv_rows(r["csv"]) == frames, "落盘行数必须等于成功帧数"


@pytest.mark.e2e
def test_first_frame_within_two_seconds():
    """回归：定时器建错线程那版会先卡 3.5 s 才出第一帧（3 s 超时 + 0.5 s 退避）。"""
    with simulator():
        t0 = time.time()
        stdout, _ = run_app("--smoke", "--seconds", "3", "--interval", "100")
    r = parse_result(stdout)
    lines = Path(r["csv"]).read_text(encoding="utf-8").splitlines()
    rows = [ln for ln in lines[1:] if ln.strip()]
    assert rows, "CSV 里没有数据行"
    ts = datetime.strptime(rows[0].split(",")[0], "%Y-%m-%d %H:%M:%S.%f")
    delay = ts.timestamp() - t0
    assert delay < 2.0, f"第一帧延迟 {delay:.2f}s，疑似连接/线程初始化问题（历史 bug 为 3.5s）"


@pytest.mark.e2e
def test_ui_load_does_not_starve_sampling():
    """界面线程完全饱和（忙等 200 ms/100 ms）时，采集节拍不能掉。"""
    seconds = 5
    with simulator():
        stdout, _ = run_app("--smoke", "--seconds", str(seconds), "--interval", "100", "--ui-load", "200")
    r = parse_result(stdout)
    frames = num(r, "ok_frames")
    expected = seconds * 1000 / 100
    assert frames >= expected * 0.7, f"UI 饱和下只采到 {frames} 帧（期望 ≈{expected:.0f}）"
    assert num(r, "timeouts") == 0


@pytest.mark.e2e
def test_scan_matrix_records_all_groups():
    """kV×mA 矩阵：3×3 必须一组不漏地落盘。"""
    with simulator():
        stdout, _ = run_app("--smoke", "--seconds", "18", "--interval", "100", "--scan")
    r = parse_result(stdout)
    assert num(r, "scan_rows") == 9, f"扫描只记下 {r['scan_rows']} 组"
    assert Path(r["scan"]).is_file()


# ── 异常路径 ──────────────────────────────────────────────────────────


@pytest.mark.e2e
def test_dropped_frames_become_timeouts_not_lost():
    """丢帧注入：必须计成超时+重试，且采集不中断。"""
    with simulator(fault="drop"):
        stdout, _ = run_app("--smoke", "--seconds", "6", "--interval", "100")
    r = parse_result(stdout)
    ticks = num(r, "ticks")
    timeouts = num(r, "timeouts")
    assert timeouts > 0, "注入了丢帧却一次超时都没记到（在途请求槽失效？）"
    assert num(r, "retries") == timeouts, "每次超时都应触发一次重试"
    # 一次请求最长占 250 ms（超时窗口），所以丢帧期间会有节拍被"在途请求"跳过；
    # 成功帧 + 超时帧只能 ≤ 节拍数，绝不能超过（超过就说明同一应答被算了两次）
    assert num(r, "ok_frames") + timeouts <= ticks, "成功帧+超时帧超过了节拍数，存在重复计数"
    assert num(r, "ok_frames") > 0, "丢帧不应导致采集整体中断"
    assert num(r, "crc_errors") == 0


@pytest.mark.e2e
def test_corrupted_frames_counted_as_crc_errors():
    """坏 CRC 注入：算 CRC 错并丢弃，不能当成功帧落盘。"""
    with simulator(fault="corrupt"):
        stdout, _ = run_app("--smoke", "--seconds", "6", "--interval", "100")
    r = parse_result(stdout)
    assert num(r, "crc_errors") > 0, "注入了坏 CRC 却一次都没识别出来"
    assert csv_rows(r["csv"]) == num(r, "ok_frames"), "坏帧不能进 CSV"


# ── 离线核对 ──────────────────────────────────────────────────────────


@pytest.mark.e2e
def test_offline_qc_matches_injected_defects(tmp_path):
    """127 组样例：判定必须稳定复现 120 通过 / 7 不合格，且集中在 CT-03、CT-05。"""
    report = tmp_path / "qc.md"
    stdout, _ = run_app("--check", str(ROOT / "testdata" / "export_5devices_127.csv"),
                        "--report", str(report))
    r = parse_result(stdout)
    assert num(r, "total") == 127
    assert num(r, "passed") == 120
    assert num(r, "failed") == 7

    text = report.read_text(encoding="utf-8")
    assert "CT-03" in text and "CT-05" in text
    body = [ln for ln in text.splitlines() if ln.startswith("|") and "CT-" in ln]
    per_device = {}
    for ln in body:
        dev = re.match(r"\|\s*\d+\s*\|\s*(CT-\d+)", ln)   # 明细表第 2 列是设备
        if dev:
            per_device[dev.group(1)] = per_device.get(dev.group(1), 0) + 1
    assert per_device.get("CT-05") == 4, f"CT-05 应该有 4 组不合格，报告里 {per_device.get('CT-05')}"
    assert per_device.get("CT-03") == 3, f"CT-03 应该有 3 组不合格，报告里 {per_device.get('CT-03')}"


@pytest.mark.e2e
def test_offline_qc_rejects_missing_input():
    """输入文件不存在：要给出可读错误并非零退出，不能崩。"""
    _require_binaries()
    out = subprocess.run(
        [str(APP), "--check", str(ROOT / "testdata" / "does_not_exist.csv")],
        cwd=str(ROOT), env=_env(), capture_output=True, text=True, timeout=60,
    )
    assert out.returncode != 0, "文件不存在却返回 0"
    combined = out.stdout + out.stderr
    assert "check_failed" in combined or "不可用" in combined or "不存在" in combined, combined
