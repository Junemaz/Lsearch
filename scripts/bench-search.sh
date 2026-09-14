#!/usr/bin/env bash
# Spec 017 基准：索引规模 / daemon RSS / 查询延迟（P50、P95）。
#
# 用法：
#   scripts/bench-search.sh                       # 复制真实 daemon 的 DB 到隔离环境（不打扰运行中的 daemon）
#   scripts/bench-search.sh --db /path/x.db       # 指定 DB
#   scripts/bench-search.sh --gen 20000           # 不用 DB，生成 20000 文件的合成树
#   scripts/bench-search.sh --queries "report .md log" --reps 7
#
# 说明：全程隔离 HOME/XDG_*，自带 daemon 生命周期与清理；只读复制 DB，绝不触碰真实 daemon。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot

DB=""
GEN=""
QUERIES="report .md log config"
REPS=7
MODE="memory"
while [ $# -gt 0 ]; do
  case "$1" in
    --db) DB="$2"; shift 2 ;;
    --gen) GEN="$2"; shift 2 ;;
    --queries) QUERIES="$2"; shift 2 ;;
    --reps) REPS="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    *) echo "未知参数：$1"; exit 2 ;;
  esac
done
case "$MODE" in
  memory|sqlite) ;;
  *) echo "未知 --mode：$MODE（应为 memory|sqlite）"; exit 2 ;;
esac

T="$ROOT/.tmp/bench-search"
rm -rf "$T"; mkdir -p "$T/home" "$T/run" "$T/config/lsearch" "$T/data/lsearch"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
DPID=""
cleanup() {
  [ -n "$DPID" ] && { kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null; }
  guard_reap_new
}
trap cleanup EXIT

if [ -n "$DB" ]; then
  [ -f "$DB" ] || { echo "DB 不存在：$DB"; exit 1; }
  cp "$DB" "$T/data/lsearch/lsearch.db"
  printf 'paths = %s\nindex_hidden = 0\nhot_index = %s\n' "$T/home" "$MODE" > "$T/config/lsearch/lsearch.conf"
  echo "==> 数据源：DB 副本（$(du -h "$T/data/lsearch/lsearch.db" | cut -f1)）"
elif [ -n "$GEN" ]; then
  python3 - "$T/home" "$GEN" <<'PY'
import os, sys
home, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    d = os.path.join(home, "d%04d" % (i // 200)); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "file_%05d.txt" % i), "wb").close()
PY
  printf 'paths = %s\nindex_hidden = 0\nhot_index = %s\n' "$T/home" "$MODE" > "$T/config/lsearch/lsearch.conf"
  echo "==> 数据源：合成树 $GEN 文件"
else
  printf 'paths = %s\nindex_hidden = 0\nhot_index = %s\n' "$T/home" "$MODE" > "$T/config/lsearch/lsearch.conf"
  echo "==> 数据源：空（仅测空索引基线）"
fi

"$ROOT/build/lsearchd" --foreground >"$T/d.log" 2>&1 &
DPID=$!
for _ in $(seq 1 200); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.1; done
[ -S "$T/run/lsearch.sock" ] || { echo "daemon 未就绪"; cat "$T/d.log"; exit 1; }
# 等索引稳定（DB 加载或首次全量扫描结束）
for _ in $(seq 1 600); do
  st="$("$ROOT/build/lsearch" --stats 2>/dev/null)"
  echo "$st" | grep -q 'rebuilding=0' && [ "$(echo "$st" | awk -F= '/^files=/{print $2}')" != "0" ] && break
  sleep 0.5
done
STATS="$("$ROOT/build/lsearch" --stats 2>/dev/null)"
FILES="$(echo "$STATS" | awk -F= '/^files=/{print $2}')"
DIRS="$(echo "$STATS" | awk -F= '/^dirs=/{print $2}')"
SIZE="$(echo "$STATS" | awk -F= '/^size=/{print $2}')"
MODE="$(echo "$STATS" | awk -F= '/^mode=/{print $2}')"
RSS_KB="$(awk '/VmRSS/{print $2}' /proc/$DPID/status 2>/dev/null)"
RSS_MIB="$(python3 -c "print(f'{$RSS_KB/1024:.1f}')")"
PER_ENTRY="$(python3 -c "n=$FILES; print(f'{$RSS_KB*1024/n:.0f}' if n else 'n/a')")"

echo
echo "索引：files=$FILES dirs=$DIRS size=$SIZE mode=${MODE:-n/a}"
echo "内存：RSS=${RSS_MIB} MiB   ≈ ${PER_ENTRY} B/项"
echo "查询延迟（lsearch --count，含进程启动 + IPC；reps=$REPS）："
for q in $QUERIES; do
  python3 - "$ROOT" "$q" "$REPS" <<'PY'
import subprocess, sys, time, statistics
root, q, reps = sys.argv[1], sys.argv[2], int(sys.argv[3])
ts, out = [], ""
for _ in range(reps):
    t0 = time.perf_counter()
    r = subprocess.run([root + "/build/lsearch", "--count", q], capture_output=True, text=True)
    ts.append((time.perf_counter() - t0) * 1000.0)
    out = (r.stdout or "").strip()
ts.sort()
p50 = statistics.median(ts)
p95 = ts[min(len(ts) - 1, int(round(0.95 * (len(ts) - 1))))]
print(f"  {q!r:14} count={out:>8}  P50={p50:6.1f} ms  P95={p95:6.1f} ms  (min={ts[0]:.1f} max={ts[-1]:.1f})")
PY
done
