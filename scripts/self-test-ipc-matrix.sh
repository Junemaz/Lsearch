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
# Spec 013 补充用例（未实现时逐条 SKIP(v13)）：
#   search/search2/search3 非法 limit（非数字 / 负数 / 超上限）-> ERR bad limit；
#   合法 limit 响应形状不变；add-path 非法路径（逗号 / TAB / 空）-> ERR bad path
#   且 get-config 与 lsearch.conf 零改动；合法 add-path 往返为单个元素；
#   remove-path 落盘（get-config 与配置文件均移除）；set-excludes _ / set-paths 回归。
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

# —— Spec 013 能力探测：limit 未校验时新增用例一律 SKIP(v13)，不使套件变红 ——
probe013, _ = talk(b"search2 abc 0 0 name - x\n")
HAS_V13 = (probe013 == b"ERR bad limit\n")
print(f"  [caps] spec013={'yes' if HAS_V13 else 'no'}")

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

# ============================================================================
# Spec 013 — IPC 输入校验与配置往返完整性（R1 / R1b / R2）
# limit 校验未就绪时（见上方探测）逐条 SKIP(v13)，既有断言不受影响。
# ============================================================================
def v13_ready(name):
    if not HAS_V13:
        skip(name, "SKIP(v13): Spec 013 未实现")
        return False
    return True

# —— R2：非法 limit -> ERR bad limit（search / search2 / search3）——
BAD_LIMITS = [("非数字", b"abc"), ("负数", b"-1"), ("超上限 1048577", b"1048577")]
for label, lim in BAD_LIMITS:
    n = f"v13 search limit {label} -> ERR bad limit"
    if v13_ready(n):
        d, _ = talk(b"search " + lim + b" 0 0 name x\n")
        (ok if d == b"ERR bad limit\n" else bad)(n, repr(d[:60]))
for label, lim in BAD_LIMITS:
    n = f"v13 search2 limit {label} -> ERR bad limit"
    if v13_ready(n):
        d, _ = talk(b"search2 " + lim + b" 0 0 name - x\n")
        (ok if d == b"ERR bad limit\n" else bad)(n, repr(d[:60]))
for label, lim in BAD_LIMITS:
    n = f"v13 search3 limit {label} -> ERR bad limit"
    if not HAS_V3:
        skip(n, "SKIP(v3): capabilities 未声明 search3")
    elif v13_ready(n):
        d, _ = talk(b"search3 " + lim + b" 0 0 name - x\n")
        (ok if d == b"ERR bad limit\n" else bad)(n, repr(d[:60]))

# —— R2：合法 limit 响应形状不变（OK <r> <t> <c> ... END）——
def v13_valid_limit(name, payload):
    if not v13_ready(name):
        return
    d, _ = talk(payload)
    first = d.split(b"\n", 1)[0]
    parts = first.decode("utf-8", "replace").split(" ")
    good = (len(parts) == 4 and parts[0] == "OK"
            and all(p.isdigit() for p in parts[1:]) and d.endswith(b"END\n"))
    (ok if good else bad)(name, first.decode("utf-8", "replace"))
v13_valid_limit("v13 search2 limit 200 响应形状不变", b"search2 200 0 0 name - x\n")
v13_valid_limit("v13 search2 limit 0 仍被接受", b"search2 0 0 0 name - x\n")

# —— R1：非法路径 -> ERR bad path，且拒绝后内存/磁盘配置零改动 ——
def cfg_get():
    d, _ = talk(b"get-config\n")
    fields = {}
    for ln in d.split(b"\n"):
        if b"=" in ln:
            k, _, v = ln.partition(b"=")
            fields[k] = v
    return d, fields

def cfg_file_bytes(fields):
    p = fields.get(b"config_file", b"").decode("utf-8", "replace")
    if not p:
        return None
    try:
        with open(p, "rb") as f:
            return f.read()
    except OSError:
        return None

base_cfg, base_fields = cfg_get()
base_bytes = cfg_file_bytes(base_fields)

for label, payload in [
    ("逗号", b"add-path /tmp/a,b\n"),
    ("TAB", b"add-path /tmp/a\tb\n"),
    ("空参数", b"add-path\n"),
]:
    n_err = f"v13 add-path {label} -> ERR bad path"
    if not v13_ready(n_err):
        continue
    d, _ = talk(payload)
    (ok if d == b"ERR bad path\n" else bad)(n_err, repr(d[:60]))
    d2, f2 = cfg_get()
    (ok if d2 == base_cfg else bad)(
        f"v13 add-path {label} 拒绝后 get-config 未变更", repr(d2[:60]))
    b2 = cfg_file_bytes(f2)
    (ok if b2 == base_bytes else bad)(
        f"v13 add-path {label} 拒绝后 lsearch.conf 字节未变更",
        f"before={len(base_bytes) if base_bytes else 0}B after={len(b2) if b2 else 0}B")

# —— R1：合法 add-path 往返为单个元素且恰好落盘一次 ——
VALID_DIR = os.path.join(HOME, "validroot")
os.makedirs(VALID_DIR, exist_ok=True)
vp = os.fsencode(VALID_DIR)

n = "v13 add-path 合法目录 -> OK"
if v13_ready(n):
    d, _ = talk(b"add-path " + vp + b"\n")
    (ok if d == b"OK\n" else bad)(n, repr(d[:60]))

    d2, f2 = cfg_get()
    paths_line = f2.get(b"paths", b"")
    single = paths_line.count(vp) == 1 and paths_line.split(b",").count(vp) == 1
    (ok if single else bad)("v13 合法路径 get-config 为单元素（未分裂）", repr(paths_line[:120]))

    b2 = cfg_file_bytes(f2) or b""
    (ok if b2.count(vp) == 1 else bad)(
        "v13 合法路径写入 lsearch.conf 恰好一次", f"count={b2.count(vp)}")

# —— R1b：remove-path 落盘（重启不复活）——
n = "v13 remove-path -> OK"
if v13_ready(n):
    d, _ = talk(b"remove-path " + vp + b"\n")
    (ok if d == b"OK\n" else bad)(n, repr(d[:60]))

    d2, f2 = cfg_get()
    paths_line = f2.get(b"paths", b"")
    (ok if vp not in paths_line.split(b",") else bad)(
        "v13 remove-path 后 get-config 不再包含", repr(paths_line[:120]))

    b2 = cfg_file_bytes(f2) or b""
    (ok if vp not in b2 else bad)(
        "v13 remove-path 后 lsearch.conf 不再包含", f"count={b2.count(vp)}")

# —— 回归：set-excludes _ 仍清空 ——
n = "v13 回归 set-excludes _ -> excludes 清空"
if v13_ready(n):
    d, _ = talk(b"set-excludes _\n")
    d2, f2 = cfg_get()
    good = (d == b"OK\n") and f2.get(b"excludes", b"") == b""
    (ok if good else bad)(n, f"resp={d[:20]!r} excl={f2.get(b'excludes', b'')[:40]!r}")

# —— 回归：合法 set-paths <dir> 仍工作 ——
n = "v13 回归 set-paths 合法目录 -> OK"
if v13_ready(n):
    d, _ = talk(b"set-paths " + vp + b"\n")
    d2, f2 = cfg_get()
    good = (d == b"OK\n") and f2.get(b"paths", b"") == vp
    (ok if good else bad)(n, f"resp={d[:20]!r} paths={f2.get(b'paths', b'')[:80]!r}")

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
