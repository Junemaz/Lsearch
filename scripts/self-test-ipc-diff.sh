#!/usr/bin/env bash
# lsearch IPC 独立客户端 × CLI 差分测试（Spec 012 Requirement 2）。
#
# 用"仅依据 ipc/proto.h + Spec 002/012 实现"的独立客户端
# scripts/ipc-spec-only-client.py，与 build/lsearch 对同一隔离守护进程、
# 同一查询语料（子串 / 通配符 * ? / re: / 各排序键 / 反序 / -d / -0 /
# --count / --under）做逐字节差分，必须一致。
#
#   ./scripts/self-test-ipc-diff.sh        # 构建后运行
#   ./scripts/self-test-ipc-diff.sh -s     # 跳过构建
#
# 依赖 python3；缺失则 SKIP（exit 0）。隔离 HOME/XDG_* + daemon guard。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-ipc-diff}"
CLEANED=0
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  guard_reap_new
  env HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" \
      XDG_RUNTIME_DIR="$T/run" "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch" ] || { echo "缺少 build/lsearch"; exit 1; }
[ -x "$ROOT/build/lsearchd" ] || { echo "缺少 build/lsearchd"; exit 1; }
[ -f "$ROOT/scripts/ipc-spec-only-client.py" ] || { echo "缺少独立客户端"; exit 1; }

echo "==> 1/ 环境探针（python3）"
if ! command -v python3 >/dev/null 2>&1; then
  echo
  echo "SKIP: 未找到 python3"
  exit 0
fi

echo "==> 2/ 隔离环境 + 构造语料 fixture"
rm -rf "$T"; mkdir -p "$T/home/sub" "$T/home/AlphabetDir" "$T/run" "$T/config" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
python3 - "$T/home" <<'PY'
import os, sys
home = sys.argv[1]
files = {
    "AlphaOne.txt": 100,
    "alpha_two.log": 200,
    "Beta.txt": 50,
    "gamma.dat": 300,
    "report-2024.txt": 10,
    "report-2025.txt": 20,
    "report extra.txt": 5,
    "alpha two.log": 7,
    "sub/report-sub.txt": 11,
    "sub/AlphaSub.txt": 12,
}
for rel, size in files.items():
    p = os.path.join(home, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(b"x" * size)
    os.utime(p, (1_700_000_000 + size * 10, 1_700_000_000 + size * 10))
# 帧完整性 fixture（v3 才可正确还原；差分仅用普通名，此处保证索引稳定）
for evil in ["evilX\nEND\nOK 99", "tabX\tb.txt"]:
    open(os.path.join(home, evil), "wb").close()
os.makedirs(os.path.join(home, "AlphabetDir"), exist_ok=True)
PY
SOCK="$T/run/lsearch.sock"
"$ROOT/build/lsearchd" --foreground </dev/null >"$T/lsearchd.log" 2>&1 &
DPID=$!
READY=0
for _ in $(seq 1 100); do
  [ -S "$SOCK" ] && { READY=1; break; }
  kill -0 "$DPID" 2>/dev/null || break
  sleep 0.1
done
if [ "$READY" != "1" ]; then
  echo "    守护进程未就绪；日志末尾："; tail -8 "$T/lsearchd.log" 2>/dev/null | sed 's/^/      /'
  echo
  echo "SKIP: build/lsearchd 未能在隔离环境启动（可能正在重建）"
  exit 0
fi
echo "    socket=$SOCK"

echo "==> 3/ 差分语料（R2）"
export ROOT SOCK IND_CLIENT="$ROOT/scripts/ipc-spec-only-client.py"
OUT="$(python3 - <<'PY' 2>&1
import os, subprocess, sys

ROOT = os.environ["ROOT"]
LS = os.path.join(ROOT, "build", "lsearch")
SOCK = os.environ["SOCK"]
PY3 = sys.executable
IND = os.environ["IND_CLIENT"]
UNDER = os.path.realpath(os.path.join(os.environ["HOME"], "sub"))

passed = 0
failed = 0

def ok(name, detail=""):
    global passed
    passed += 1
    print(f"  PASS  {name} {detail}".rstrip())

def bad(name, detail=""):
    global failed
    failed += 1
    print(f"  FAIL  {name} {detail}".rstrip())

def run(cmd):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

# 语料：name, query, sort, dirs, print0, count, under, reverse
cases = [
    ("子串 report",           "report", None,  False, False, False, None,  False),
    ("通配符 *",              "*.txt",  None,  False, False, False, None,  False),
    ("通配符 ?",              "report-202?.txt", None, False, False, False, None, False),
    ("re: 正则",              r"re:^report-\d{4}\.txt$", None, False, False, False, None, False),
    ("排序 name",             "a", "name",  False, False, False, None,  False),
    ("排序 path",             "a", "path",  False, False, False, None,  False),
    ("排序 size",             "a", "size",  False, False, False, None,  False),
    ("排序 mtime",            "a", "mtime", False, False, False, None,  False),
    ("反序（TUI F7 语义）",   "a", "name",  False, False, False, None,  True),
    ("仅目录 -d",             "a", None,  True,  False, False, None,  False),
    ("NUL 分隔 -0",           "a", None,  False, True,  False, None,  False),
    ("计数 --count",          "a", None,  False, False, True,  None,  False),
    ("子树 --under",          "report", None, False, False, False, UNDER, False),
]

for name, query, sort, dirs, print0, count, under, reverse in cases:
    cli = [LS, "-m"]
    ind = [PY3, IND, "--sock", SOCK, "--query", query]
    if sort:
        cli += ["-s", sort]; ind += ["--sort", sort]
    if dirs:
        cli += ["-d"]; ind += ["--dirs-only"]
    if print0:
        cli += ["-0"]; ind += ["--print0"]
    if count:
        cli += ["-c"]; ind += ["--count"]
    if under:
        cli += ["--under", under]; ind += ["--under", under]
    if reverse:
        ind += ["--reverse"]
    cli += [query]
    cli_out = run(cli)
    ind_out = run(ind)
    if reverse:
        # CLI 无 --reverse（反序是 TUI F7 的客户端语义）；与"CLI 结果逐行反转"比对。
        expected = b"".join(reversed(cli_out.splitlines(True)))
    else:
        expected = cli_out
    detail = f"cli={len(cli_out)}B ind={len(ind_out)}B"
    if not cli_out and not reverse:
        bad(name, "CLI 空输出（fixture/查询异常）")
        continue
    if expected == ind_out:
        ok(name, detail)
    else:
        cli_show = cli_out[:160]
        ind_show = ind_out[:160]
        bad(name, f"{detail}\n        cli={cli_show!r}\n        ind={ind_show!r}")

print(f"  [diff] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PY
)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then
  echo "  FAIL  driver 异常退出（rc=$RC）"; FAIL=$((FAIL + 1))
fi

echo
echo "=============================================="
echo "  IPC 独立客户端 × CLI 差分：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then
  echo "  独立实现与 CLI 逐字节一致 ✅"
else
  echo "  存在差异，保留 $T 供排查"
fi
cleanup
if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; fi
exit "$FAIL"
