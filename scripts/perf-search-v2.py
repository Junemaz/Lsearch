#!/usr/bin/env python3
"""Spec 010 R6 perf harness: measure search/search2/count2 latency over the IPC socket.

Usage:
  LSEARCH_PERF_SOCK=/path/lsearch.sock LSEARCH_PERF_PID=<daemon-pid> \
    python3 scripts/perf-search-v2.py [iterations]

Prints p50/p95 (ms) per scenario plus daemon VmHWM. Requires python3 only.
"""
import base64
import os
import socket
import sys

SOCK = os.environ.get("LSEARCH_PERF_SOCK") or ""
PID = os.environ.get("LSEARCH_PERF_PID", "")
ITERS = int(sys.argv[1]) if len(sys.argv) > 1 else 25


def send(req):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(SOCK)
    f = s.makefile("rwb")
    f.write(req.encode() + b"\n")
    f.flush()
    first = f.readline().decode().rstrip()
    if req.startswith(("search ", "search2 ")):
        while True:
            if f.readline().decode().rstrip() == "END":
                break
    s.close()
    return first


def stats_files():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(SOCK)
    f = s.makefile("rwb")
    f.write(b"stats\n")
    f.flush()
    files = 0
    while True:
        line = f.readline().decode().rstrip()
        if line == "END":
            break
        if line.startswith("files="):
            files = int(line[6:])
    s.close()
    return files


def vm_hwm_kb():
    if not PID:
        return None
    try:
        with open(f"/proc/{PID}/status") as fh:
            for line in fh:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


import time  # noqa: E402

under_ws = base64.b64encode(b"/home/code/Lsearch").decode()
scenarios = [
    ("legacy search wide 'a'", "search 200 0 0 name a"),
    ("search2 substring wide 'a'", "search2 200 0 0 name - a"),
    ("search2 substring narrow 'main.cpp'", "search2 200 0 0 name - main.cpp"),
    ("search2 + under '/home/code/Lsearch' 'lsearch'",
     f"search2 200 0 0 name {under_ws} lsearch"),
    ("count2 wide 'a'", "count2 0 0 - a"),
    ("count2 narrow 'main.cpp'", "count2 0 0 - main.cpp"),
    ("regex wide 're:.'", "search2 200 0 0 name - re:."),
]


def percentile(vals, p):
    vals = sorted(vals)
    idx = max(0, min(len(vals) - 1, int(round(p / 100.0 * len(vals) + 0.5)) - 1))
    return vals[idx]


def main():
    if not SOCK:
        print("LSEARCH_PERF_SOCK is required", file=sys.stderr)
        return 2
    files = stats_files()
    print(f"index_files={files} iterations={ITERS} cpu={os.cpu_count()}")
    print(f"rss_before_kb={vm_hwm_kb()}")
    print()
    print("| scenario | p50 (ms) | p95 (ms) | threshold |")
    print("|---|---|---|---|")
    thresholds = {
        "legacy search wide 'a'": "< 10",
        "search2 substring wide 'a'": "< 100",
        "search2 substring narrow 'main.cpp'": "< 30",
        "search2 + under '/home/code/Lsearch' 'lsearch'": "< 30",
        "count2 wide 'a'": "< 30",
        "count2 narrow 'main.cpp'": "< 30",
        "regex wide 're:.'": "< 250 (or excluded, Spec 008)",
    }
    for name, req in scenarios:
        for _ in range(3):  # warmup
            send(req)
        samples = []
        for _ in range(ITERS):
            t0 = time.perf_counter()
            send(req)
            samples.append((time.perf_counter() - t0) * 1000.0)
        print(f"| {name} | {percentile(samples,50):.2f} | {percentile(samples,95):.2f} | "
              f"{thresholds[name]} |")
    print()
    print(f"rss_after_wide_kb={vm_hwm_kb()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
