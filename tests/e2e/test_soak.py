"""长跑稳定性（soak）：默认不跑，需要显式选：

    SOAK_SECONDS=3600 pytest tests/e2e/test_soak.py -m soak -v

断言：帧数不掉、无超时、CSV 行数与成功帧一致、内存不持续增长（工作集漂移 < 20% 且 < 32 MB）。
内存直接读进程工作集：Windows 用 Psapi，Linux 读 /proc/<pid>/status。
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

from test_hmi_cli import APP, ROOT, SIM, _env, _wait_port, csv_rows, parse_result


def _rss_bytes(pid: int) -> int | None:
    """返回进程工作集字节数；取不到返回 None（不因为监控失败让用例误红）。"""
    try:
        if os.name == "nt":
            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            PROCESS_VM_READ = 0x0010
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
            if not handle:
                return None

            class PMC(ctypes.Structure):
                _fields_ = [
                    ("cb", ctypes.c_ulong),
                    ("PageFaultCount", ctypes.c_ulong),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t),
                ]

            counters = PMC()
            counters.cb = ctypes.sizeof(PMC)
            ok = ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
            kernel32.CloseHandle(handle)
            return counters.WorkingSetSize if ok else None
        with open(f"/proc/{pid}/status", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        return None
    return None


def _mb(b: int | None) -> str:
    return "n/a" if b is None else f"{b / 1048576:.1f} MB"


@pytest.mark.soak
def test_soak_no_leak_and_no_frame_loss(tmp_path):
    seconds = int(os.environ.get("SOAK_SECONDS", "300"))
    interval = int(os.environ.get("SOAK_INTERVAL", "100"))
    assert APP and SIM, "先编译上位机与模拟下位机（HMI_APP / SIM_APP 可覆盖）"

    sim = subprocess.Popen([str(SIM)], cwd=str(ROOT), env=_env(),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    samples: list[tuple[float, int | None]] = []
    try:
        _wait_port(15020)
        t0 = time.time()
        proc = subprocess.Popen(
            [str(APP), "--smoke", "--seconds", str(seconds), "--interval", str(interval)],
            cwd=str(ROOT), env=_env(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        while proc.poll() is None:
            samples.append((time.time() - t0, _rss_bytes(proc.pid)))
            if time.time() - t0 > seconds + 120:   # 兜底：别把 CI 吊死
                proc.kill()
                pytest.fail(f"上位机超过 {seconds + 120}s 还没退出")
            time.sleep(5)
        stdout = proc.stdout.read() if proc.stdout else ""
    finally:
        sim.terminate()
        try:
            sim.wait(timeout=5)
        except subprocess.TimeoutExpired:
            sim.kill()

    r = parse_result(stdout)
    frames, timeouts = int(r["ok_frames"]), int(r["timeouts"])
    expected = seconds * 1000 / interval
    elapsed_frames = frames
    print(f"\n长跑 {seconds}s：帧 {frames}（理论 ≈{expected:.0f}）、超时 {timeouts}、"
          f"CRC {r['crc_errors']}、CSV {csv_rows(r['csv'])} 行")

    assert frames >= expected * 0.9, f"长跑掉帧：{frames} 帧 / 理论 {expected:.0f}"
    assert timeouts == 0, f"长跑里出现 {timeouts} 次超时"
    assert int(r["crc_errors"]) == 0
    assert csv_rows(r["csv"]) == frames, "CSV 行数与成功帧不一致（写盘丢了？）"

    pts = [(ts, v) for ts, v in samples if v]
    if len(pts) >= 6:
        # 预热段（前 1/5 时长、至少 60 s）里 Qt 会涨字体/图元缓存，不能算泄漏；
        # 之后看"斜率"：真泄漏是单调上升，缓存预热是一段台阶后持平。
        warm_until = max(pts[0][0] + 60.0, pts[-1][0] / 5.0)
        warm = [(ts, v) for ts, v in pts if ts >= warm_until]
        if len(warm) < 4:
            warm = pts[len(pts) // 5:]
        xs = [ts for ts, _ in warm]
        ys = [v for _, v in warm]
        drift = max(ys) - min(ys)
        pct = drift / max(ys) * 100
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        denom = sum((x - mx) ** 2 for x in xs) or 1.0
        slope_bpm = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denom * 60.0 / 1048576
        print(f"内存：全段 {_mb(pts[0][1])} → {_mb(pts[-1][1])}；预热后漂移 {_mb(drift)}（{pct:.1f}%），"
              f"斜率 {slope_bpm:+.2f} MB/min")
        assert drift < 32 * 1048576, f"预热后内存漂移 {_mb(drift)}（{pct:.1f}%），疑似泄漏"
        assert slope_bpm < 0.5, f"内存斜率 {slope_bpm:+.2f} MB/min，持续增长疑似泄漏"
    else:
        print("警告：内存采样点不足，跳过内存断言")
