#!/usr/bin/env bash
# Lsearch IPC 协议 v2 端到端自测（Unix socket 明文协议，P1–P13）
#   ./scripts/self-test-ipc.sh        # 自动构建后跑全部
#   ./scripts/self-test-ipc.sh -s     # 跳过构建，使用现有 build/
# 覆盖：capabilities / search2（含 under）/ count2 / 非法与超长 base64 / 控制字符 /
#       缺字段 / 非法正则 / 旧 search 响应格式不变。
# 说明：守护进程与客户端须同 shell 环境；隔离 HOME/XDG_*；python3 仅作 socket 驱动。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-ipc}"
PASS=0; FAIL=0

DPID=""
CLEANED=0
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  if [ -n "$DPID" ]; then
    "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1
    wait "$DPID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearchd" ] || { echo "缺少 build/lsearchd"; exit 1; }

echo "==> 1/ 隔离测试环境（$T）"
rm -rf "$T"
mkdir -p "$T/home/docs" "$T/home/pics" "$T/run"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
export ROOT LSEARCH_SOCK="$T/run/lsearch.sock"
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/docs/dup_same.txt"
echo x > "$T/home/pics/dup_same.txt"

echo "==> 2/ 启动守护进程并建索引"
"$ROOT/build/lsearchd" --foreground > "$T/log.txt" 2>&1 &
DPID=$!
for i in $(seq 1 80); do [ -S "$LSEARCH_SOCK" ] && break; sleep 0.2; done
sleep 1
if [ ! -S "$LSEARCH_SOCK" ]; then echo "守护进程未就绪"; exit 1; fi

echo "==> 3/ 驱动 IPC（P1–P13）"
cat > "$T/driver.py" <<'PYEOF'
import base64, os, socket, sys

SOCK = os.environ["LSEARCH_SOCK"]
HOME = os.environ["HOME"]

passed = 0
failed = 0
def ok(name, msg=""):
    global passed; passed += 1; print(f"  PASS  {name} {msg}".rstrip())
def bad(name, msg=""):
    global failed; failed += 1; print(f"  FAIL  {name} {msg}".rstrip())

class Raw:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(15)
        self.s.connect(SOCK)
        self.f = self.s.makefile("rwb")
    def send(self, line):
        self.f.write(line.encode() + b"\n")
        self.f.flush()
    def readline(self):
        raw = self.f.readline()
        if not raw:
            raise RuntimeError("unexpected EOF from lsearchd")
        return raw.decode(errors="replace").rstrip("\n")
    def read_until_end(self):
        lines = []
        while True:
            line = self.readline()
            if line == "END":
                break
            lines.append(line)
        return lines

def b64(s):
    return base64.b64encode(s.encode()).decode()

r = Raw()

# ---- P1: capabilities ----
try:
    r.send("capabilities")
    first = r.readline()
    body = r.read_until_end()
    cmds = []
    for line in body:
        if line.startswith("commands="):
            cmds = line[len("commands="):].split(",")
    wants = {"search", "search2", "count2"}
    okv = first == "OK" and wants.issubset(set(cmds))
    (ok if okv else bad)("P1", "capabilities 含 search/search2/count2" if okv else f"{first} {body}")
except Exception as e:
    bad("P1", f"exception: {e}")

# ---- P2: search2 无 under ----
try:
    r.send("search2 200 0 0 name - dup_same")
    first = r.readline()
    rows = r.read_until_end()
    parts = first.split()
    okv = (parts[0] == "OK" and int(parts[1]) == len(rows) == 2
           and int(parts[2]) == 2 and parts[3] == "0")
    (ok if okv else bad)("P2", "search2 无 under → returned=total=2, capped=0"
                        if okv else f"{first} rows={len(rows)}")
except Exception as e:
    bad("P2", f"exception: {e}")

# ---- P3: search2 + under（base64）----
try:
    docs = os.path.join(HOME, "docs")
    r.send(f"search2 200 0 0 name {b64(docs)} dup_same")
    first = r.readline()
    rows = r.read_until_end()
    parts = first.split()
    okv = (parts[0] == "OK" and int(parts[2]) == 1 and len(rows) == 1
           and rows[0].split("\t")[0] == os.path.join(docs, "dup_same.txt"))
    (ok if okv else bad)("P3", "search2 under=docs → total=1 且仅 docs 路径"
                        if okv else f"{first} rows={rows}")
except Exception as e:
    bad("P3", f"exception: {e}")

# ---- P4: count2 ----
try:
    r.send("count2 0 0 - dup_same")
    first = r.readline()
    parts = first.split()
    okv = parts == ["OK", "2", "0"]
    (ok if okv else bad)("P4", "count2 全量 → OK 2 0" if okv else f"{first}")

    docs = os.path.join(HOME, "docs")
    r.send(f"count2 0 0 {b64(docs)} dup_same")
    first = r.readline()
    parts = first.split()
    okv = parts == ["OK", "1", "0"]
    (ok if okv else bad)("P5", "count2 under=docs → OK 1 0" if okv else f"{first}")
except Exception as e:
    bad("P4", f"exception: {e}")

# ---- P6/P7/P8/P9: 非法/超长 base64、控制字符、缺字段 → ERR bad under ----
try:
    r.send("search2 200 0 0 name *** dup_same")
    okv = r.readline() == "ERR bad under"
    (ok if okv else bad)("P6", "search2 非法 base64 → ERR bad under")

    r.send("search2 200 0 0 name " + "A" * 9000 + " dup_same")
    okv = r.readline() == "ERR bad under"
    (ok if okv else bad)("P7", "search2 超长 base64(>8192) → ERR bad under")

    r.send("search2 200 0 0 name " + b64("/a\nb") + " dup_same")
    okv = r.readline() == "ERR bad under"
    (ok if okv else bad)("P8", "search2 解码含控制字符 → ERR bad under")

    r.send("search2 200 0 0 name")
    okv = r.readline() == "ERR bad under"
    (ok if okv else bad)("P9", "search2 缺字段 → ERR bad under")
except Exception as e:
    bad("P6", f"exception: {e}")

# ---- P10/P11: count2 非法 under / 缺字段 ----
try:
    r.send("count2 0 0 *** dup_same")
    okv = r.readline() == "ERR bad under"
    (ok if okv else bad)("P10", "count2 非法 base64 → ERR bad under")

    r.send("count2 0 0")
    okv = r.readline() == "ERR bad under"
    (ok if okv else bad)("P11", "count2 缺字段 → ERR bad under")
except Exception as e:
    bad("P10", f"exception: {e}")

# ---- P12: search2 非法正则沿用 bad regex ----
try:
    r.send("search2 200 0 0 name - re:[")
    first = r.readline()
    okv = first.startswith("ERR bad regex")
    (ok if okv else bad)("P12", "search2 非法正则 → ERR bad regex" if okv else f"{first}")
except Exception as e:
    bad("P12", f"exception: {e}")

# ---- P13: 旧 search 响应格式不变（首行 OK <count>，行 + END）----
try:
    r.send("search 200 0 0 name AnnualReport")
    first = r.readline()
    rows = r.read_until_end()
    want = os.path.join(HOME, "docs", "AnnualReport.txt")
    okv = (first == "OK 1" and len(rows) == 1 and rows[0].split("\t")[0] == want)
    (ok if okv else bad)("P13", "旧 search 响应 'OK <count>'+行+END 不变"
                        if okv else f"{first} rows={rows}")
except Exception as e:
    bad("P13", f"exception: {e}")

print(f"  [ipc-driver] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PYEOF

OUT="$(python3 "$T/driver.py" 2>&1)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then echo "  FAIL  driver 异常退出（rc=$RC）"; FAIL=1; fi

echo
echo "=============================================="
echo "  IPC 自测：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  IPC 协议全部通过 ✅"; else echo "  存在问题，请核对上方 FAIL 项"; fi

cleanup
rm -rf "$T"
exit "$FAIL"
