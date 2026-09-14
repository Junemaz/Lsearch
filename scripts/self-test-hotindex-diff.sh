#!/usr/bin/env bash
# Spec 017 差分测试：hot_index=memory 与 hot_index=sqlite 结果逐字节一致。
#
# 同一隔离索引上先用 memory 模式跑一遍查询语料，关停后切到 sqlite 模式
# 复用同一 DB 再跑一遍，逐字节比对 stdout + 退出码（含 --under / glob / re: /
# 各排序键 / --count / -d / -f / -0 / --details）。
#
#   ./scripts/self-test-hotindex-diff.sh        # 构建后运行
#   ./scripts/self-test-hotindex-diff.sh -s     # 跳过构建
#
# 依赖 python3；隔离 HOME/XDG_* + daemon guard，绝不触碰真实 daemon。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot

SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-hotindex-diff}"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

cleanup(){
  env HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" \
      XDG_RUNTIME_DIR="$T/run" "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
  guard_reap_new
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch" ] || { echo "缺少 build/lsearch"; exit 1; }
[ -x "$ROOT/build/lsearchd" ] || { echo "缺少 build/lsearchd"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: 未找到 python3"; exit 0; }

echo "==> 1/ 隔离环境 + 构造语料"
rm -rf "$T"
mkdir -p "$T/home/sub/deep" "$T/home/AlphabetDir" "$T/run" "$T/config/lsearch" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
python3 - "$T/home" <<'PY'
import os, sys
home = sys.argv[1]
files = {
    "Report-Final.txt": 321,
    "report-2024.txt": 11,
    "report-2025.txt": 22,
    "Report extra space.md": 7,
    "AnnualReport.txt": 5,
    "alpha_two.log": 200,
    "AlphaOne.txt": 100,
    "beta.TXT": 50,
    "gamma.dat": 300,
    "报告-2026.txt": 42,
    "sub/report-sub.txt": 13,
    "sub/Report-Sub2.md": 17,
    "sub/deep/report-deep.txt": 19,
    "AlphabetDir/zeta.txt": 3,
    "AlphabetDir/eta.md": 2,
    ".hidden_note.txt": 9,
}
for rel, size in files.items():
    p = os.path.join(home, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(b"x" * size)
    os.utime(p, (1_700_000_000 + size * 37, 1_700_000_000 + size * 37))
PY

write_conf(){
  printf 'paths = %s\nindex_hidden = 1\nhot_index = %s\n' "$T/home" "$1" \
      > "$T/config/lsearch/lsearch.conf"
}

start_daemon(){
  local mode="$1"
  "$ROOT/build/lsearchd" --foreground </dev/null >"$T/lsearchd-$mode.log" 2>&1 &
  DPID=$!
  for _ in $(seq 1 200); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.1; done
  [ -S "$T/run/lsearch.sock" ] || { echo "daemon($mode) 未就绪"; tail -8 "$T/lsearchd-$mode.log"; exit 1; }
  for _ in $(seq 1 200); do
    st="$("$ROOT/build/lsearch" -m --stats 2>/dev/null)"
    echo "$st" | grep -q 'rebuilding=0' && [ "$(echo "$st" | awk -F= '/^files=/{print $2}')" != "0" ] && break
    sleep 0.2
  done
}

stop_daemon(){
  "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
  for _ in $(seq 1 100); do [ -S "$T/run/lsearch.sock" ] || break; sleep 0.1; done
  wait "$DPID" 2>/dev/null || true
}

echo "==> 2/ 生成查询语料并按 memory 模式采集"
run_cases(){
  python3 - "$ROOT/build/lsearch" "$1" <<'PY'
import base64, os, socket, subprocess, sys
LS, dest = sys.argv[1], sys.argv[2]
HOME = os.environ["HOME"]
SOCK = os.environ["XDG_RUNTIME_DIR"] + "/lsearch.sock"
UNDER = os.path.realpath(os.path.join(HOME, "sub"))
cases = [
    ("substr_lower", ["report", "-l", "0"]),
    ("substr_upper", ["REPORT", "-l", "0"]),
    ("substr_cjk", ["报告", "-l", "0"]),
    ("substr_absent", ["zzz_absent", "-l", "0"]),
    ("glob_star", ["*.txt", "-l", "0"]),
    ("glob_question", ["rep?rt*", "-l", "0"]),
    ("regex_anchor", ["re:^report", "-l", "0"]),
    ("regex_digits", ["re:[0-9]{3}", "-l", "0"]),
    ("regex_cjk", ["re:报告", "-l", "0"]),
    ("sort_name", ["e", "-l", "0", "-s", "name"]),
    ("sort_path", ["e", "-l", "0", "-s", "path"]),
    ("sort_size", ["a", "-l", "0", "-s", "size"]),
    ("sort_mtime", ["a", "-l", "0", "-s", "mtime"]),
    ("dirs_only", ["e", "-l", "0", "-d"]),
    ("files_only", ["e", "-l", "0", "-f"]),
    ("print0", ["report", "-l", "0", "-0"]),
    ("details", ["report", "-l", "0", "-S"]),
    ("count_substr", ["--count", "report"]),
    ("count_regex", ["--count", "re:rep"]),
    ("count_dirs", ["--count", "-d", "e"]),
    ("count_under", ["--count", "--under", UNDER, "report"]),
    ("under_substr", ["--under", UNDER, "report", "-l", "0"]),
    ("under_glob", ["--under", UNDER, "*.txt", "-l", "0"]),
    ("under_regex", ["--under", UNDER, "re:rep", "-l", "0"]),
    ("under_sort_size", ["--under", UNDER, ".", "-l", "0", "-s", "size"]),
    ("under_sort_mtime", ["--under", UNDER, ".", "-l", "0", "-s", "mtime"]),
    ("under_dirs", ["--under", UNDER, "-d", ".", "-l", "0"]),
]
os.makedirs(dest, exist_ok=True)
for name, args in cases:
    r = subprocess.run([LS, "-m"] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    with open(os.path.join(dest, name + ".out"), "wb") as f:
        f.write(r.stdout)
    with open(os.path.join(dest, name + ".rc"), "w") as f:
        f.write(str(r.returncode))

# 原始协议覆盖 legacy `search` 命令（CLI 已改走 search3，该路径仅老客户端使用）。
def raw(req, end_marker):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(SOCK)
    s.sendall(req.encode() + b"\n")
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
        if end_marker and data.endswith(b"END\n"):
            break
        if not end_marker and b"\n" in data:
            break
    s.close()
    return data

under64 = base64.b64encode(UNDER.encode()).decode()
raw_cases = [
    ("raw_search_name", "search 0 0 0 name report", True),
    ("raw_search_size", "search 0 0 0 size report", True),
    ("raw_search2_name", "search2 0 0 0 name - report", True),
    ("raw_search3_name", "search3 0 0 0 name - report", True),
    ("raw_count2", "count2 0 0 - report", False),
    ("raw_count2_under", "count2 0 0 " + under64 + " report", False),
]
for name, req, end_marker in raw_cases:
    with open(os.path.join(dest, name + ".out"), "wb") as f:
        f.write(raw(req, end_marker))
    with open(os.path.join(dest, name + ".rc"), "w") as f:
        f.write("0")
print(len(cases) + len(raw_cases))
PY
}

write_conf memory
start_daemon memory
MEM_MODE="$("$ROOT/build/lsearch" -m --stats 2>/dev/null | awk -F= '/^mode=/{print $2}')"
[ "$MEM_MODE" = "memory" ] && ok "memory 模式 stats.mode=memory" || bad "memory 模式 stats.mode=$MEM_MODE"
N="$(run_cases "$T/cases-memory")"
ok "memory 模式采集 $N 个用例"
stop_daemon

echo "==> 3/ 切换 hot_index=sqlite（复用同一 DB）并采集"
write_conf sqlite
start_daemon sqlite
SQL_MODE="$("$ROOT/build/lsearch" -m --stats 2>/dev/null | awk -F= '/^mode=/{print $2}')"
[ "$SQL_MODE" = "sqlite" ] && ok "sqlite 模式 stats.mode=sqlite" || bad "sqlite 模式 stats.mode=$SQL_MODE"
run_cases "$T/cases-sqlite" >/dev/null
stop_daemon

echo "==> 4/ 逐字节比对（stdout + 退出码）"
for f in "$T"/cases-memory/*.out; do
  name="$(basename "$f" .out)"
  if cmp -s "$T/cases-memory/$name.out" "$T/cases-sqlite/$name.out" \
     && cmp -s "$T/cases-memory/$name.rc" "$T/cases-sqlite/$name.rc"; then
    ok "一致：$name"
  else
    bad "不一致：$name"
    echo "      mem: $(head -c 160 "$T/cases-memory/$name.out" | tr '\n' ' ')"
    echo "      sql: $(head -c 160 "$T/cases-sqlite/$name.out" | tr '\n' ' ')"
  fi
done

echo
echo "=============================================="
echo "  通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  memory/sqlite 结果逐字节一致 ✅"; else echo "  存在不一致，请核对上方 FAIL 项"; fi
exit "$FAIL"
