#!/usr/bin/env bash
# Lsearch MCP 前端端到端自测（stdio JSON-RPC，S1–S11）
#   ./scripts/self-test-mcp.sh        # 自动构建后跑全部
#   ./scripts/self-test-mcp.sh -s     # 跳过构建，使用现有 build/
# 说明：守护进程与 MCP 客户端必须处于同一 shell 环境（/tmp 隔离），故全部在此脚本内完成；
#       隔离 HOME/XDG_* 指向工作区临时目录，Python3 仅作 JSON-RPC 驱动。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-mcp}"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

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
[ -x "$ROOT/build/lsearch-mcp" ] || { echo "缺少 build/lsearch-mcp"; exit 1; }

echo "==> 1/ 隔离测试环境（$T）"
rm -rf "$T"
mkdir -p "$T/home/docs" "$T/home/pics" "$T/run"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
export ROOT MCP_STDERR="$T/mcp-stderr.txt"
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/pics/vacation_photo.jpg"
echo x > "$T/home/readme.md"
echo x > "$T/home/docs/mcp_pg_1.txt"
echo x > "$T/home/docs/mcp_pg_2.txt"
echo x > "$T/home/docs/mcp_pg_3.txt"
echo x > "$T/home/docs/mcp_pg_4.txt"
echo x > "$T/home/docs/mcp_pg_5.txt"
echo x > "$T/home/docs/mcp_pg_6.txt"

echo "==> 2/ 启动守护进程并建索引"
"$ROOT/build/lsearchd" --foreground > "$T/log.txt" 2>&1 &
DPID=$!
for i in $(seq 1 80); do [ -S "$T/run/lsearch.sock" ] && break; sleep 0.2; done
sleep 1
if [ ! -S "$T/run/lsearch.sock" ]; then echo "守护进程未就绪"; exit 1; fi

echo "==> 3/ 驱动 MCP（S1–S11）"
cat > "$T/driver.py" <<'PYEOF'
import json, os, subprocess, sys

ROOT = os.environ["ROOT"]
HOME = os.environ["HOME"]
MCP = os.path.join(ROOT, "build", "lsearch-mcp")
LSEARCH = os.path.join(ROOT, "build", "lsearch")
STDERR = os.environ["MCP_STDERR"]

passed = 0
failed = 0
def ok(name, msg=""):
    global passed; passed += 1; print(f"  PASS  {name} {msg}".rstrip())
def bad(name, msg=""):
    global failed; failed += 1; print(f"  FAIL  {name} {msg}".rstrip())

def modern_meta():
    return {"_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {},
    }}

class McpClient:
    def __init__(self):
        self.err = open(STDERR, "wb")
        self.p = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err)
        self.lines = []
        self.next_id = 1
    def send(self, obj):
        self.p.stdin.write(json.dumps(obj).encode() + b"\n")
        self.p.stdin.flush()
    def recv(self):
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("unexpected EOF from lsearch-mcp")
        self.lines.append(line)
        return json.loads(line.decode())
    def request(self, method, params=None, meta=None, rid=None):
        if rid is None:
            rid = self.next_id; self.next_id += 1
        req = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            req["params"] = params
        if meta is not None:
            req.setdefault("params", {})
            req["params"].update(meta)
        self.send(req)
        return self.recv()
    def notify(self, method, params=None):
        req = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            req["params"] = params
        self.send(req)
    def request_raw(self, raw):
        self.p.stdin.write(raw.encode() + b"\n")
        self.p.stdin.flush()
        return self.recv()

def result(r):
    return r.get("result")

def errcode(r):
    return (r.get("error") or {}).get("code")

def call_search(c, args):
    return c.request("tools/call", params={"name": "search_files", "arguments": args})

def search_payload(r):
    return json.loads(result(r)["content"][0]["text"])

c = McpClient()

# ---- S1: server/discover 前置可达 ----
try:
    r = c.request("server/discover", meta=modern_meta())
    res = result(r)
    okv = (res is not None and "2026-07-28" in res.get("supportedVersions", [])
           and res.get("resultType") == "complete")
    (ok if okv else bad)("S1", "discover supportedVersions+resultType" if okv else f"{r}")
except Exception as e:
    bad("S1", f"exception: {e}")

# ---- S2: modern tools/list 带 _meta ----
try:
    r = c.request("tools/list", meta=modern_meta())
    res = result(r) or {}
    names = [t.get("name") for t in res.get("tools", [])]
    okv = ("ttlMs" in res and res.get("cacheScope") == "public"
           and "search_files" in names and "index_stats" in names)
    (ok if okv else bad)("S2", "tools/list ttlMs+cacheScope+2 tools" if okv else f"{r}")
except Exception as e:
    bad("S2", f"exception: {e}")

# ---- S3: legacy initialize + search_files ----
try:
    r = c.request("initialize", params={"protocolVersion": "2025-11-25",
                                        "capabilities": {},
                                        "clientInfo": {"name": "selftest", "version": "0"}})
    res = result(r) or {}
    okv = (res.get("protocolVersion") == "2025-11-25"
           and isinstance((res.get("capabilities") or {}).get("tools"), dict))
    (ok if okv else bad)("S3a", "initialize 回显版本+capabilities" if okv else f"{r}")
    c.notify("notifications/initialized")
    r = call_search(c, {"query": "report"})
    res = result(r) or {}
    payload = json.loads(res["content"][0]["text"])
    paths = [x["path"] for x in payload["results"]]
    okv = any(p.endswith("AnnualReport.txt") for p in paths) and res.get("isError") is False
    (ok if okv else bad)("S3b", "search_files{report} 命中 AnnualReport.txt" if okv else f"{r}")
except Exception as e:
    bad("S3", f"exception: {e}")

# ---- S4: 非法参数 / 空查询 / limit 极端值 ----
try:
    r = call_search(c, {"query": ""})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S4a", "空查询 -32602" if okv else f"{r}")

    r = c.request("tools/call", params={"name": "shutdown"})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S4b", "未知工具 -32602" if okv else f"{r}")

    payload = search_payload(call_search(c, {"query": "mcp_pg", "limit": 0}))
    okv = payload["page"]["limit"] == 1 and payload["page"]["returned"] <= 1
    (ok if okv else bad)("S4c", "limit=0 → 钳制 1，无洪水" if okv else f"{payload['page']}")

    payload = search_payload(call_search(c, {"query": "mcp_pg", "limit": 1000000}))
    okv = payload["page"]["limit"] == 200 and payload["page"]["returned"] <= 200
    (ok if okv else bad)("S4d", "limit=1000000 → 钳制 200，无洪水" if okv else f"{payload['page']}")

    r = call_search(c, {"query": "x\nshutdown"})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S4e", "query 含换行 → -32602（命令注入被拒）" if okv else f"{r}")

    r = c.request("tools/call", params={"name": "index_stats"})
    payload = json.loads(result(r)["content"][0]["text"])
    okv = payload.get("files", 0) > 0
    (ok if okv else bad)("S4f", "注入尝试后 daemon 仍存活" if okv else f"{payload}")
except Exception as e:
    bad("S4", f"exception: {e}")

# ---- S5: 分页切片 + has_more 边界 + 50000 cap ----
try:
    glob = search_payload(call_search(c, {"query": "mcp_pg", "limit": 200}))
    global_names = [x["name"] for x in glob["results"]]
    p0 = search_payload(call_search(c, {"query": "mcp_pg", "offset": 0, "limit": 2}))
    p1 = search_payload(call_search(c, {"query": "mcp_pg", "offset": 2, "limit": 2}))
    a = [x["name"] for x in p0["results"]]
    b = [x["name"] for x in p1["results"]]
    okv = (len(global_names) >= 6 and len(a) == 2 and len(b) == 2
           and p0["page"]["has_more"] is True and p1["page"]["has_more"] is True
           and not (set(a) & set(b))
           and set(a) | set(b) == set(global_names[:4]))
    (ok if okv else bad)("S5a", "page(0,2)∪page(2,2)=全局前 4 且不重叠"
                        if okv else f"global={global_names} A={a} B={b}")

    cap = search_payload(call_search(c, {"query": "mcp_pg", "offset": 50000, "limit": 2}))
    okv = (cap["page"]["returned"] == 0 and cap["page"]["has_more"] is False
           and cap["page"]["truncated"] is True)
    (ok if okv else bad)("S5b", "offset≥50000 → 空页/truncated" if okv else f"{cap['page']}")
except Exception as e:
    bad("S5", f"exception: {e}")

# ---- S6: index_stats ----
try:
    r = c.request("tools/call", params={"name": "index_stats"})
    res = result(r) or {}
    payload = json.loads(res["content"][0]["text"])
    okv = payload.get("files", 0) > 0 and HOME in payload.get("roots", "")
    (ok if okv else bad)("S6", "index_stats files>0 且 roots 含隔离 HOME" if okv else f"{payload}")
except Exception as e:
    bad("S6", f"exception: {e}")

# ---- S11: 正则匹配（re: 前缀）----
try:
    payload = search_payload(call_search(c, {"query": r"re:^AnnualReport\.txt$"}))
    names = [x["name"] for x in payload["results"]]
    okv = names == ["AnnualReport.txt"]
    (ok if okv else bad)("S11a", "search_files re:^AnnualReport\\.txt$ → AnnualReport.txt"
                        if okv else f"{names}")
except Exception as e:
    bad("S11a", f"exception: {e}")

try:
    r = call_search(c, {"query": "re:["})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S11b", "search_files re:[ → -32602" if okv else f"{r}")
except Exception as e:
    bad("S11b", f"exception: {e}")

try:
    r = call_search(c, {"query": "re:"})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S11c", "search_files re: → -32602（空查询规则）" if okv else f"{r}")
except Exception as e:
    bad("S11c", f"exception: {e}")

# ---- S7: 只读工具面 ----
try:
    res = result(c.request("tools/list")) or {}
    names = [t.get("name") for t in res.get("tools", [])]
    forbidden = {"shutdown", "rebuild", "add-path", "remove-path",
                 "set-paths", "set-excludes", "set-opts"}
    okv = not any(n in forbidden or n.startswith("set-") for n in names)
    (ok if okv else bad)("S7a", "无写操作工具" if okv else f"{names}")

    r = c.request("tools/call", params={"name": "shutdown"})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S7b", "调用 shutdown → -32602" if okv else f"{r}")
except Exception as e:
    bad("S7", f"exception: {e}")

# ---- S10: 协议健壮性（非法 _meta / 批次 / 非有限数 / 超长行 / ping）----
try:
    r = c.request("tools/list",
                  params={"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}})
    okv = errcode(r) == -32602
    (ok if okv else bad)("S10a", "present-but-invalid _meta → -32602" if okv else f"{r}")
except Exception as e:
    bad("S10a", f"exception: {e}")

try:
    r = c.request_raw('[{"jsonrpc":"2.0","id":1,"method":"ping"}]')
    okv = isinstance(r, dict) and errcode(r) == -32600
    (ok if okv else bad)("S10b", "JSON 数组批次 → -32600" if okv else f"{r}")
except Exception as e:
    bad("S10b", f"exception: {e}")

try:
    r = c.request_raw('{"jsonrpc":"2.0","id":1e999,"method":"ping"}')
    okv = isinstance(r, dict) and r.get("jsonrpc") == "2.0" and errcode(r) == -32700
    (ok if okv else bad)("S10c", "id:1e999 → 合法 JSON 响应（-32700）" if okv else f"{r}")
except Exception as e:
    bad("S10c", f"exception: {e}")

try:
    r = c.request("ping")
    okv = isinstance(r, dict) and result(r) == {}
    (ok if okv else bad)("S10d", "ping → result {}" if okv else f"{r}")
except Exception as e:
    bad("S10d", f"exception: {e}")

try:
    r = c.request_raw("a" * (1024 * 1024 + 64))
    okv = isinstance(r, dict) and errcode(r) == -32700
    (ok if okv else bad)("S10e", "超长行 → -32700" if okv else f"{r}")
    r = c.request("ping")
    okv = isinstance(r, dict) and result(r) == {}
    (ok if okv else bad)("S10f", "超长行后仍可服务" if okv else f"{r}")
except Exception as e:
    bad("S10e", f"exception: {e}")

# ---- S9: 未知方法 + stdout 纯净性 ----
try:
    r = c.request("no/such_method", meta=modern_meta())
    okv = errcode(r) == -32601
    (ok if okv else bad)("S9a", "未知方法 → -32601" if okv else f"{r}")
except Exception as e:
    bad("S9a", f"exception: {e}")

# ---- S8: stdin EOF → 退出 0，守护进程存活 ----
try:
    c.p.stdin.close()
    rc = c.p.wait(timeout=15)
    okv = rc == 0
    (ok if okv else bad)("S8a", "stdin EOF → MCP 退出码 0" if okv else f"rc={rc}")
    q = subprocess.run([LSEARCH, "-m", "report"], capture_output=True, text=True,
                       env=os.environ)
    okv = q.returncode == 0 and "AnnualReport.txt" in q.stdout
    (ok if okv else bad)("S8b", "MCP 退出后 lsearch 仍可用（daemon 存活）"
                        if okv else f"rc={q.returncode} out={q.stdout!r}")
except Exception as e:
    bad("S8", f"exception: {e}")

# ---- S9b: 所有 stdout 行均为合法 JSON-RPC；日志仅在 stderr ----
try:
    all_json = len(c.lines) > 0
    for raw in c.lines:
        try:
            obj = json.loads(raw.decode())
            if not isinstance(obj, dict) or obj.get("jsonrpc") != "2.0":
                all_json = False
        except Exception:
            all_json = False
    c.err.flush()
    stderr_size = os.path.getsize(STDERR)
    okv = all_json and stderr_size > 0
    (ok if okv else bad)("S9b", "stdout 全为 JSON-RPC 且日志在 stderr"
                        if okv else f"lines={len(c.lines)} stderr={stderr_size}")
except Exception as e:
    bad("S9b", f"exception: {e}")

print(f"  [mcp-driver] passed={passed} failed={failed}")
sys.exit(1 if failed else 0)
PYEOF

OUT="$(python3 "$T/driver.py" 2>&1)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then bad "driver 异常退出（rc=$RC）"; fi

echo
echo "=============================================="
echo "  MCP 自测：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  MCP 前端全部通过 ✅"; else echo "  存在问题，请核对上方 FAIL 项"; fi

cleanup
rm -rf "$T"
exit "$FAIL"