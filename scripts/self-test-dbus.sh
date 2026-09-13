#!/usr/bin/env bash
# Lsearch D-Bus 桥接端到端自测（dbus-run-session + gdbus，D1–D22）
#   ./scripts/self-test-dbus.sh        # 自动构建后跑全部
#   ./scripts/self-test-dbus.sh -s     # 跳过构建，使用现有 build/
# 说明：必须把 service 文件写入隔离的 $XDG_DATA_HOME/dbus-1/services 后**再**启动
#       dbus-run-session，故外层准备环境、内层在会话总线中做全部断言；隔离 HOME/XDG_*。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKIP_BUILD=0
[ "${1:-}" = "-s" ] && SKIP_BUILD=1
T="${LSEARCH_TEST_DIR:-$ROOT/.selftest-dbus}"
PASS=0; FAIL=0

CLEANED=0
cleanup(){
  [ "$CLEANED" = "1" ] && return
  CLEANED=1
  "$ROOT/build/lsearchd" --shutdown >/dev/null 2>&1 || true
  pkill -x lsearch-dbus >/dev/null 2>&1 || true
  rm -rf "$T"
}
trap cleanup EXIT

echo "==> 0/ 构建"
if [ "$SKIP_BUILD" = "0" ]; then
  cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
fi
[ -x "$ROOT/build/lsearch-dbus" ] || { echo "缺少 build/lsearch-dbus"; exit 1; }
[ -x "$ROOT/build/lsearchd" ] || { echo "缺少 build/lsearchd"; exit 1; }
command -v dbus-run-session >/dev/null 2>&1 || { echo "缺少 dbus-run-session"; exit 1; }
command -v gdbus >/dev/null 2>&1 || { echo "缺少 gdbus"; exit 1; }

echo "==> 1/ 隔离测试环境（$T）"
rm -rf "$T"
mkdir -p "$T/home/docs" "$T/home/pics" "$T/run" "$T/data/dbus-1/services"
export HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_RUNTIME_DIR="$T/run"
export PATH="$ROOT/build:$PATH"
export ROOT T
echo x > "$T/home/docs/AnnualReport.txt"
echo x > "$T/home/pics/vacation_photo.jpg"
echo x > "$T/home/readme.md"

# 按需激活：service 文件必须在 dbus-daemon 启动前就位，Exec 指向 build 绝对路径。
cat > "$T/data/dbus-1/services/com.lsearch.Daemon.service" <<EOF
[D-BUS Service]
Name=com.lsearch.Daemon
Exec=$ROOT/build/lsearch-dbus
EOF

cat > "$T/inner.sh" <<'INNER'
#!/usr/bin/env bash
# 运行于 dbus-run-session 内（隔离 HOME/XDG_* 已由外层导出）。
set -uo pipefail
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

D="com.lsearch.Daemon"
P="/com/lsearch/Daemon"
I="com.lsearch.Daemon1"
CALL(){ gdbus call --session --dest "$D" --object-path "$P" --method "$I.$1" "${@:2}"; }
snap(){ pgrep -x lsearchd 2>/dev/null | sort -n | tr '\n' ' '; }

BPID=""
cleanup_inner(){ [ -n "$BPID" ] && kill "$BPID" 2>/dev/null || true; pkill -x lsearch-dbus 2>/dev/null || true; }
trap cleanup_inner EXIT

# ---- 激活前置：必须无桥接、无预启动 daemon、名字无 owner ----
BEFORE_D="$(snap)"
if pgrep -x lsearch-dbus >/dev/null 2>&1; then bad "D1 调用前不应有 lsearch-dbus 进程"; else ok "D1 调用前无 lsearch-dbus 进程"; fi
[ ! -S "$XDG_RUNTIME_DIR/lsearch.sock" ] && ok "D2 调用前隔离 socket 不存在（无预启动 daemon）" || bad "D2 隔离 socket 已存在"
OUT="$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner "$D" 2>&1)"
case "$OUT" in *false*) ok "D3 调用前 NameHasOwner=false";; *) bad "D3 NameHasOwner（得到：$OUT）";; esac

# ---- 首次 Search 触发 D-Bus 按需激活 ----
OUT="$(CALL Search "report" 20 0 name any 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] && printf '%s' "$OUT" | grep -q "AnnualReport.txt"; then
  ok "D4 Search 触发激活并命中 AnnualReport.txt"
else
  bad "D4 Search（rc=$RC out=$OUT）"
fi

BPID="$(pgrep -x lsearch-dbus | head -1)"
if [ -n "$BPID" ]; then ok "D5 调用后 lsearch-dbus 存在（pid=$BPID）"; else bad "D5 未发现桥接进程"; fi

# 激活权威证明：总线报告的 name owner PID 必须就是该桥接进程。dbus-daemon 在服务成功
# 取到名字后会回收 activation babysitter，故服务可能被 reparent（本机实测为 init）；
# owner PID 比 PPID 更可靠地证明"由 D-Bus 激活"。
OWNER_OUT="$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
             --method org.freedesktop.DBus.GetConnectionUnixProcessID "$D" 2>&1)"
OWNER_PID="$(printf '%s' "$OWNER_OUT" | grep -oE '[0-9]+' | tail -1)"
if [ -n "$BPID" ] && [ "$OWNER_PID" = "$BPID" ]; then
  ok "D6 总线报告的 name owner PID == 桥接 PID（$OWNER_PID，D-Bus 激活证明）"
else
  bad "D6 owner PID（owner=$OWNER_PID bpid=$BPID out=$OWNER_OUT）"
fi
if [ -n "$BPID" ]; then
  PP="$(ps -o ppid= -p "$BPID" 2>/dev/null | tr -d ' ')"
  PCOMM="$(ps -o comm= -p "$PP" 2>/dev/null | tr -d ' ')"
  if [ "$PP" != "$$" ] && [ "$PP" != "$PPID" ]; then
    ok "D7 桥接非测试脚本直接启动（父进程 $PCOMM pid=$PP）"
  else
    bad "D7 桥接父进程疑似测试脚本（comm=$PCOMM pp=$PP）"
  fi
else
  bad "D7 无桥接进程可检查"
fi

AFTER_D="$(snap)"
TEST_DPID=""
for p in $AFTER_D; do
  case " $BEFORE_D " in *" $p "*) ;; *) TEST_DPID="$p";; esac
done
if [ -n "$TEST_DPID" ] && readlink -f "/proc/$TEST_DPID/exe" 2>/dev/null | grep -q "build/lsearchd"; then
  ok "D8 lsearchd 由桥接拉起且为 build 版本（pid=$TEST_DPID）"
else
  bad "D8 新 lsearchd 未出现或路径不符（pid=$TEST_DPID exe=$(readlink -f "/proc/$TEST_DPID/exe" 2>/dev/null)）"
fi

# ---- 只读方法 ----
OUT="$(CALL Stats 2>&1)"
if printf '%s' "$OUT" | grep -q "'files'" && printf '%s' "$OUT" | grep -q "'rebuilding'"; then
  ok "D9 Stats 返回 a{sv} 含 files/rebuilding 键"
else
  bad "D9 Stats（$OUT）"
fi
OUT="$(CALL Version 2>&1)"
if printf '%s' "$OUT" | grep -q "0.1.0"; then ok "D10 Version 非空且含桥接版本"; else bad "D10 Version（$OUT）"; fi
OUT="$(gdbus call --session --dest "$D" --object-path "$P" \
        --method org.freedesktop.DBus.Introspectable.Introspect 2>&1)"
if printf '%s' "$OUT" | grep -q "com.lsearch.Daemon1"; then ok "D11 Introspect 返回接口 XML"; else bad "D11 Introspect（$OUT）"; fi

# ---- 错误用例：全部 InvalidArgs，桥接与 daemon 不受影响 ----
expect_invalid(){
  local name="$1"; shift
  local out rc
  out="$(CALL Search "$@" 2>&1)"; rc=$?
  if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "com.lsearch.Error.InvalidArgs"; then
    ok "$name"
  else
    bad "$name（rc=$rc out=$out）"
  fi
}
expect_invalid "D12 非法 sort → InvalidArgs" "q" 20 0 bogus any
expect_invalid "D13 空 query → InvalidArgs" "" 20 0 name any
expect_invalid "D14 非法正则 re:[ → InvalidArgs" "re:[" 20 0 name any
expect_invalid "D15 query 含换行 → InvalidArgs（注入防护）" $'x\nshutdown' 20 0 name any
if kill -0 "$TEST_DPID" 2>/dev/null; then ok "D16 非法/注入请求后 lsearchd 仍存活"; else bad "D16 lsearchd 受影响"; fi

# ---- kill 桥接后重新激活：新桥接 PID ≠ 旧，daemon PID 不变 ----
DAEMONS_BEFORE_KILL="$(snap)"
OLD_BPID="$BPID"
kill "$OLD_BPID" 2>/dev/null || true
for _ in $(seq 1 50); do kill -0 "$OLD_BPID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$OLD_BPID" 2>/dev/null; then bad "D17 旧桥接未退出"; else ok "D17 旧桥接已退出"; fi

OUT="$(CALL Search "report" 20 0 name any 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] && printf '%s' "$OUT" | grep -q "AnnualReport.txt"; then
  ok "D18 kill 后再次调用成功（D-Bus 重新激活）"
else
  bad "D18 重新激活（rc=$RC out=$OUT）"
fi
NEW_BPID="$(pgrep -x lsearch-dbus | head -1)"
if [ -n "$NEW_BPID" ] && [ "$NEW_BPID" != "$OLD_BPID" ]; then
  ok "D19 新桥接 PID ≠ 旧（$OLD_BPID → $NEW_BPID）"
else
  bad "D19 桥接 PID（old=$OLD_BPID new=$NEW_BPID）"
fi
DAEMONS_AFTER="$(snap)"
if [ "$DAEMONS_BEFORE_KILL" = "$DAEMONS_AFTER" ]; then
  ok "D20 lsearchd PID 集合不变（未被重启）"
else
  bad "D20 lsearchd 集合变化（before=$DAEMONS_BEFORE_KILL after=$DAEMONS_AFTER）"
fi

# ---- Rebuild 桥接侧节流：首次成功，5s 内再次 → Busy ----
OUT="$(CALL Rebuild 2>&1)"; RC=$?
if [ "$RC" -eq 0 ]; then ok "D21 Rebuild 首次触发成功"; else bad "D21 Rebuild（rc=$RC out=$OUT）"; fi
OUT="$(CALL Rebuild 2>&1)"; RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q "com.lsearch.Error.Busy"; then
  ok "D22 5s 内再次 Rebuild → com.lsearch.Error.Busy"
else
  bad "D22 Rebuild 节流（rc=$RC out=$OUT）"
fi

echo "  [dbus-driver] passed=$PASS failed=$FAIL"
exit $([ "$FAIL" -gt 0 ] && echo 1 || echo 0)
INNER

OUT="$(dbus-run-session -- bash "$T/inner.sh" 2>&1)"; RC=$?
printf '%s\n' "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^  PASS' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -c '^  FAIL' || true)
if [ "$RC" -ne 0 ] && [ "$FAIL" -eq 0 ]; then
  FAIL=$((FAIL+1)); echo "  FAIL  inner 异常退出（rc=$RC）"
fi

echo
echo "=============================================="
echo "  D-Bus 自测：通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then echo "  D-Bus 桥接全部通过 ✅"; else echo "  存在问题，请核对上方 FAIL 项"; fi

cleanup
exit "$FAIL"
