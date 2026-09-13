#!/usr/bin/env bash
# Lsearch IPC 协议 v2 端到端自测（Unix socket 明文协议，P1–P18）
#   ./scripts/self-test-ipc.sh        # 自动构建后跑全部
#   ./scripts/self-test-ipc.sh -s     # 跳过构建，使用现有 build/
# 覆盖：capabilities / search2（含 under）/ count2 / 非法与超长 base64 / 控制字符 /
#       缺字段 / 非法正则 / 旧 search 响应格式不变 /
#       旧 daemon 降级（无 under 静默降级、有 under 显式报错，CLI 与 MCP）。
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

echo "==> 4/ 旧 daemon 降级（fake stub，P14–P18）"
mkdir -p "$T/stub-run"
cat > "$T/stub.py" <<'PYEOF'
import json, os, socket, subprocess, sys, threading, time

ROOT = os.environ["ROOT"]
SOCK = os.environ["STUB_SOCK"]
CLI = os.path.join(ROOT, "build", "lsearch")
MCP = os.path.join(ROOT, "build", "lsearch-mcp")

passed = 0
failed = 0
def ok(name, msg=""):
    global passed; passed += 1; print(f"  PASS  {name} {msg}".rstrip())
def bad(name, msg=""):
    global failed; failed += 1; print(f"  FAIL  {name} {msg}".rstrip())

def serve():
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        os.unlink(SOCK)
    except OSError:
        pass
    srv.bind(SOCK)
    srv.listen(16)
    while True:
        try:
            c, _ = srv.accept()
        except OSError:
            return
        f = c.makefile("rwb")
        try:
            while True:
                line = f.readline()
                if not line:
                    break
                cmd = line.decode(errors="replace").strip()
                if cmd == "ping":
                    resp = "OK pong\n"
                elif cmd.startswith("search "):
                    resp = "OK 1\n/tmp/stub_hit.txt\t0\t7\t12345\t0\nEND\n"
                else:
                    resp = "ERR unknown command\n"
                f.write(resp.encode())
                f.flush()
        except OSError:
            pass
        finally:
            c.close()

os.makedirs(os.path.dirname(SOCK), exist_ok=True)
t = threading.Thread(target=serve, daemon=True)
t.start()
for _ in range(100):
    if os.path.exists(SOCK):
        break
    time.sleep(0.05)

env = dict(os.environ)

def cli(args):
    return subprocess.run([CLI, "-m"] + args, capture_output=True, text=True, env=env)

# P14: 无 under → legacy search 正常（静默降级保留）
try:
    r = cli(["foo"])
    okv = r.returncode == 0 and "/tmp/stub_hit.txt" in r.stdout
    (ok if okv else bad)("P14", "旧 daemon 无 under → 普通 search 成功（降级保留）"
                        if okv else f"rc={r.returncode} out={r.stdout!r} err={r.stderr!r}")
except Exception as e:
    bad("P14", f"exception: {e}")

# P15: under + 旧 daemon → 显式失败（不静默丢弃）
try:
    r = cli(["--under", "/tmp", "foo"])
    okv = r.returncode == 2 and "does not support 'under'" in r.stderr
    (ok if okv else bad)("P15", "旧 daemon + under 搜索 → 退出 2 + 可操作错误"
                        if okv else f"rc={r.returncode} err={r.stderr!r}")
except Exception as e:
    bad("P15", f"exception: {e}")

# P16: under + --count + 旧 daemon → 显式失败
try:
    r = cli(["--under", "/tmp", "--count", "foo"])
    okv = r.returncode == 2 and "does not support 'under'" in r.stderr
    (ok if okv else bad)("P16", "旧 daemon + under --count → 退出 2 + 可操作错误"
                        if okv else f"rc={r.returncode} err={r.stderr!r}")
except Exception as e:
    bad("P16", f"exception: {e}")

def mcp_call(args):
    p = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=env)
    req = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
           "params": {"name": "search_files", "arguments": args}}
    out, _ = p.communicate((json.dumps(req) + "\n").encode(), timeout=30)
    return json.loads(out.decode().splitlines()[0])

# P17: MCP under + 旧 daemon → isError:true + 可操作错误
try:
    r = mcp_call({"query": "foo", "under": "/tmp"})
    res = r.get("result") or {}
    text = res.get("content", [{}])[0].get("text", "")
    okv = res.get("isError") is True and "does not support 'under'" in text
    (ok if okv else bad)("P17", "MCP 旧 daemon + under → isError:true"
                        if okv else f"{r}")
except Exception as e:
    bad("P17", f"exception: {e}")

# P18: MCP 无 under + 旧 daemon → 正常；legacy page.total_is_lower_bound=true
try:
    r = mcp_call({"query": "foo"})
    res = r.get("result") or {}
    payload = json.loads(res["content"][0]["text"])
    okv = (res.get("isError") is False and payload["page"]["total_is_lower_bound"] is True
           and payload["results"][0]["path"] == "/tmp/stub_hit.txt")
    (ok if okv else bad)("P18", "MCP 旧 daemon 无 under → 成功 + total_is_lower_bound=true"
                        if okv else f"{payload['page']}")
except Exception as e:
    bad("P18", f"exception: {e}")

print(f"  [stub-driver] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PYEOF

export STUB_SOCK="$T/stub-run/lsearch.sock"
OLD_RUNTIME="$XDG_RUNTIME_DIR"
export XDG_RUNTIME_DIR="$T/stub-run"
OUT2="$(python3 "$T/stub.py" 2>&1)"; RC2=$?
printf '%s\n' "$OUT2"
P2=$(printf '%s\n' "$OUT2" | grep -c '^  PASS' || true)
F2=$(printf '%s\n' "$OUT2" | grep -c '^  FAIL' || true)
PASS=$((PASS + P2)); FAIL=$((FAIL + F2))
if [ "$RC2" -ne 0 ] && [ "$F2" -eq 0 ]; then echo "  FAIL  stub driver 异常退出（rc=$RC2）"; FAIL=$((FAIL + 1)); fi
export XDG_RUNTIME_DIR="$OLD_RUNTIME"

echo
echo "=============================================="
echo "  IPC 自测：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  IPC 协议全部通过 ✅"; else echo "  存在问题，请核对上方 FAIL 项"; fi

cleanup
rm -rf "$T"
exit "$FAIL"
