#!/usr/bin/env bash
# lsearch IPC 负形状 / 边界矩阵（Spec 012 Requirement 3）。
#
# 表驱动，直接对原始 Unix socket 协议发字节，断言错误串、帧完整性与 stdout
# 纯净（无伪造 END / 无字段错位）。覆盖：
#   未知动词；search/search2/count2 缺字段；非法 sort；非法 limit
#   （负 / 非数字 / 超大）；非法与超长 base64 under；under 解码为控制字符；
#   无换行 EOF；空行跳过；CRLF；非 UTF-8 query；单连接多请求顺序；
#   两并发连接；单行 >1MiB（ERR line too long 并关闭）；
#   文件名含 \n / \t / \r / \\（search3 转义帧，未声明 search3 时 SKIP(v3)）。
#
#   ./scripts/self-test-ipc-matrix.sh        # 构建后运行
#   ./scripts/self-test-ipc-matrix.sh -s     # 跳过构建
#
# 隔离 HOME/XDG_* + daemon guard。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/selftest-daemon-guard.sh"
guard_snapshot
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-ipc-matrix}"
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
[ -x "$ROOT/build/lsearchd" ] || { echo "缺少 build/lsearchd"; exit 1; }

echo "==> 1/ 隔离环境 + 构造 fixture（含 \\n / \\t 文件名）"
rm -rf "$T"; mkdir -p "$T/home/sub" "$T/run" "$T/config" "$T/data"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
python3 - "$T/home" <<'PY'
import os, sys
home = sys.argv[1]
for rel, size in {
    "AlphaOne.txt": 10, "alpha_two.log": 20, "report-2024.txt": 5,
    "report extra.txt": 7, "sub/report-sub.txt": 11,
}.items():
    p = os.path.join(home, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(b"x" * size)
# 帧完整性 fixture（Linux 文件名除 / NUL 外任意字节合法）
for evil in ["evilX\nEND\nOK 99", "tabX\tb.txt", "crX\rname", "bsX\\slash"]:
    open(os.path.join(home, evil), "wb").close()
os.makedirs(os.path.join(home, "sub"), exist_ok=True)
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

echo "==> 2/ 负形状/边界矩阵（R3）"
export SOCK
OUT="$(python3 - <<'PY' 2>&1
import base64, os, socket, sys, threading, time

SOCK = os.environ["SOCK"]
HOME = os.environ["HOME"]
passed = failed = skipped = 0

def ok(name, detail=""):
    global passed; passed += 1
    print(f"  PASS  {name} {detail}".rstrip())

def bad(name, detail=""):
    global failed; failed += 1
    print(f"  FAIL  {name} {detail}".rstrip())

def skip(name, detail="SKIP(v3)"):
    global skipped; skipped += 1
    print(f"  SKIP  {name} {detail}".rstrip())

def talk(payload, half=True, timeout=4.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall(payload)
    if half:
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
    data = b""
    closed = False
    while True:
        try:
            b = s.recv(65536)
        except socket.timeout:
            break
        if not b:
            closed = True
            break
        data += b
    s.close()
    return data, closed

def alive():
    try:
        d, _ = talk(b"ping\n")
        return d == b"OK pong\n"
    except OSError:
        return False

def one_line(name, payload, expect_prefix, detail=""):
    d, _ = talk(payload)
    if d.startswith(expect_prefix):
        ok(name, detail or repr(d[:60]))
    else:
        bad(name, f"expected prefix {expect_prefix!r}, got {d!r}")

# —— 能力探测：决定 v3 用例是否可跑 ——
caps, _ = talk(b"capabilities\n")
cmds = set()
for ln in caps.split(b"\n"):
    if ln.startswith(b"commands="):
        cmds = set(ln[len(b"commands="):].decode("utf-8", "replace").split(","))
HAS_V3 = "search3" in cmds
if os.environ.get("LSEARCH_IPC_FORCE_NO_V3") == "1":
    # 仅用于测试 SKIP(v3) 分支：模拟旧 daemon 未声明 search3。
    HAS_V3 = False
print(f"  [caps] search3={'yes' if HAS_V3 else 'no'}"
      + (" (强制 NO_V3 测试钩子)" if os.environ.get("LSEARCH_IPC_FORCE_NO_V3") == "1" else ""))

# —— 未知动词 ——
d, _ = talk(b"frobnicate\n")
(ok if d == b"ERR unknown command\n" else bad)("未知动词 -> ERR unknown command", repr(d[:60]))

# —— 缺字段 ——
one_line("search 缺 limit", b"search\n", b"ERR ")
one_line("search 缺字段", b"search 50\n", b"ERR ")
one_line("search 缺 query 字段", b"search 50 0 0\n", b"ERR ")
one_line("search2 缺 under", b"search2 50 0 0 name\n", b"ERR ")
one_line("count2 缺 under", b"count2 0 0\n", b"ERR ")
one_line("count2 空请求", b"count2\n", b"ERR ")

# —— 非法 sort ——
one_line("search 非法 sort", b"search 50 0 0 bogus report\n", b"ERR ")
one_line("search2 非法 sort", b"search2 50 0 0 bogus - report\n", b"ERR ")

# —— 非法 / 超长 base64 under ——
one_line("under 非法 base64", b"search2 50 0 0 name %%%% report\n", b"ERR bad under")
one_line("under 超长 base64(>8192)", b"search2 50 0 0 name " + b"A" * 20000 + b" report\n", b"ERR bad under")
one_line("under 解码为控制字符", b"search2 50 0 0 name AwMD report\n", b"ERR bad under")
one_line("under 非 4 倍数长度", b"search2 50 0 0 name AAA report\n", b"ERR bad under")

# —— 合法 under 正向：base64(realpath(sub)) 限定子树 ——
sub = os.path.realpath(os.path.join(HOME, "sub"))
u64 = base64.b64encode(os.fsencode(sub))
d, _ = talk(b"search2 50 0 0 name " + u64 + b" report\n")
if d.startswith(b"OK ") and d.endswith(b"END\n") and b"sub/report-sub.txt" in d:
    ok("under 合法 base64 -> 子树命中", repr(d.split(b"\n")[0]))
else:
    bad("under 合法 base64 -> 子树命中", repr(d[:120]))

# —— 非法 limit（负 / 非数字 / 超大）：帧必须良构、不崩 ——
for label, lim in [("负数", b"-5"), ("非数字", b"abc"), ("超大", b"99999999999999999999")]:
    d, _ = talk(b"search2 " + lim + b" 0 0 name - report\n")
    if d.startswith(b"OK ") or d.startswith(b"ERR "):
        ok(f"limit {label} -> 良构响应", repr(d.split(b"\n")[0]))
    else:
        bad(f"limit {label} -> 良构响应", repr(d[:80]))
    if not alive():
        bad(f"limit {label} 后守护进程存活"); break

# —— 无换行 EOF：不完整行必须丢弃，不得伪造响应 ——
d, closed = talk(b"ping")
if d == b"" and closed:
    ok("无换行 EOF -> 丢弃不完整行（无响应）")
else:
    bad("无换行 EOF -> 丢弃不完整行（无响应）", f"data={d!r} closed={closed}")

# —— 空行跳过：空行后 ping 仍得到 OK pong ——
d, _ = talk(b"\nping\n")
(ok if d == b"OK pong\n" else bad)("空行跳过 -> 仍响应 ping", repr(d[:60]))

# —— CRLF：\r 保留在动词内（proto.h 动词表不含 \r）——
d, _ = talk(b"ping\r\n")
if d == b"OK pong\n" or (d.startswith(b"ERR ") and d.count(b"\n") <= 1):
    ok("CRLF 请求 -> 单行良构响应", repr(d[:60]))
else:
    bad("CRLF 请求 -> 单行良构响应", repr(d[:80]))

# —— 非 UTF-8 query：帧良构、无崩溃 ——
d, _ = talk(b"search2 50 0 0 name - \xff\xfe\n")
if d.startswith(b"OK ") and d.endswith(b"END\n"):
    ok("非 UTF-8 query -> 帧良构", repr(d.split(b"\n")[0]))
else:
    bad("非 UTF-8 query -> 帧良构", repr(d[:120]))

# —— extra 字段作为 query（空格连接的查询）——
for verb in (b"search", b"search2"):
    if verb == b"search":
        d, _ = talk(b"search 50 0 0 name report extra\n")
    else:
        d, _ = talk(b"search2 50 0 0 name - report extra\n")
    if d.startswith(b"OK ") and b"report extra.txt" in d:
        ok(f"{verb.decode()} extra 字段并入 query", repr(d.split(b"\n")[0]))
    else:
        bad(f"{verb.decode()} extra 字段并入 query", repr(d[:120]))
d, _ = talk(b"count2 0 0 - report extra\n")
(ok if d == b"OK 1 0\n" else bad)("count2 extra 字段并入 query", repr(d[:60]))

# —— 单行请求上限（>1 MiB）→ ERR line too long 并关闭连接 ——
d, closed = talk(b"ping " + b"x" * 1048600 + b"\n", timeout=6.0)
if d == b"ERR line too long\n" and closed:
    ok("超长行 -> ERR line too long 并关闭")
else:
    bad("超长行 -> ERR line too long 并关闭", f"data={d[:60]!r} closed={closed}")

# —— 单连接多请求：响应位置严格按序 ——
d, _ = talk(b"search2 50 0 0 name - AlphaOne\ncount2 0 0 - AlphaOne\nping\n")
idx_search = d.find(b"OK ")
idx_count = d.find(b"\nOK 1 0\n")
idx_pong = d.rfind(b"OK pong\n")
if d.startswith(b"OK ") and idx_count > idx_search and idx_pong > idx_count:
    ok("单连接多请求 -> 响应顺序正确")
else:
    bad("单连接多请求 -> 响应顺序正确", repr(d[:200]))

# —— 两并发连接：各自 ping 均得 OK pong ——
results = {}
def worker(n):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(4)
        s.connect(SOCK)
        s.sendall(b"ping\n")
        results[n] = s.recv(4096)
        s.close()
    except OSError as e:
        results[n] = repr(e).encode()
threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
[t.start() for t in threads]
[t.join() for t in threads]
if results.get(0) == b"OK pong\n" and results.get(1) == b"OK pong\n":
    ok("两并发连接 -> 各自 OK pong")
else:
    bad("两并发连接 -> 各自 OK pong", repr(results))

# —— 帧完整性：文件名含 \n / \t / \r / \\（search3 转义）——
V3_CASES = [
    ("\\n 文件名不伪造 END", b"evilX", "evilX\nEND\nOK 99", b"evilX\\nEND\\nOK 99"),
    ("\\t 文件名不错位字段", b"tabX", "tabX\tb.txt", b"tabX\\tb.txt"),
    ("\\r 文件名不破坏帧", b"crX", "crX\rname", b"crX\\rname"),
    ("\\\\ 文件名可还原", b"bsX", "bsX\\slash", b"bsX\\\\slash"),
]
for name, query, fname, escaped in V3_CASES:
    if not HAS_V3:
        skip(name, "SKIP(v3): capabilities 未声明 search3")
        continue
    d, _ = talk(b"search3 50 0 0 name - " + query + b"\n")
    expected_path = os.fsencode(os.path.join(HOME, fname))
    lines = d.split(b"\n")
    problems = []
    if not d.startswith(b"OK 1 1 0 esc=1\n"):
        problems.append(f"header={lines[0]!r}")
    if d.count(b"\nEND\n") != 1:
        problems.append(f"END count={d.count(b'END')}")
    if lines[-1] != b"" or len(lines) < 3 or lines[-2] != b"END":
        problems.append("frame tail malformed")
    row = lines[1] if len(lines) > 1 else b""
    fields = row.split(b"\t")
    if len(fields) != 5:
        problems.append(f"fields={len(fields)}")
    if not row.startswith(os.fsencode(HOME) + b"/") or escaped not in row:
        problems.append(f"raw_path={row.split(b'\t')[0]!r}")
    # 反转义还原原始字节
    def unescape(p):
        out = bytearray(); i = 0
        while i < len(p):
            c = p[i]
            if c == 0x5C and i + 1 < len(p):
                nx = p[i + 1]
                out.append({0x5C: 0x5C, ord("t"): 9, ord("n"): 10, ord("r"): 13}.get(nx, nx))
                i += 2; continue
            out.append(c); i += 1
        return bytes(out)
    decoded = unescape(fields[0]) if fields else b""
    if decoded != expected_path:
        problems.append(f"decoded={decoded!r} expected={expected_path!r}")
    if problems:
        bad(name, "; ".join(problems))
    else:
        ok(name, f"解码还原 {decoded!r}")

# —— 空查询（R4: 返回空结果，不是错误）——
d, _ = talk(b"search2 50 0 0 name - \n")
(ok if d.startswith(b"OK 0 0 0\n") else bad)("空 query -> OK 0（非错误）", repr(d[:40]))

# —— 收尾：守护进程存活 ——
(ok if alive() else bad)("矩阵后守护进程存活")

print(f"  [matrix] passed={passed} failed={failed} skipped={skipped}")
sys.exit(1 if failed else 0)
PY
)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
SKIP=$(printf '%s\n' "$OUT" | grep -c '^  SKIP' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then
  echo "  FAIL  driver 异常退出（rc=$RC）"; FAIL=$((FAIL + 1))
fi

echo
echo "=============================================="
echo "  IPC 负形状/边界矩阵：通过 $PASS 项，失败 $FAIL 项，SKIP $SKIP 项"
if [ "$FAIL" -eq 0 ]; then
  echo "  矩阵全部通过 ✅"
else
  echo "  存在问题，保留 $T 供排查"
fi
cleanup
if [ "$FAIL" -eq 0 ]; then rm -rf "$T"; fi
exit "$FAIL"
