#!/usr/bin/env bash
# ============================================================================
# run_e2e.sh — 端到端测试编排（设计文档 §9.3，全组件版）
#
# 场景：
#   1. 真实 HS256 签名 token 全链路（客户端现场签名 @sign）
#   2. 调试后门 token（--allow-debug-token，debug:user-1）
#   3. wasm 认证（错误 token 被拒 + wasm-ok 放行）
#   4. Redis 持久化断言（online:{uid} 键存在）
#   5. /metrics Prometheus 抓取断言
#   6. 崩溃转储断言（SIGSEGV → crash_*.txt 生成）
# ============================================================================
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD_DIR:-build-wsl}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

WS_PORT=9443
METRICS_PORT=19099
BACKEND=127.0.0.1:50051
REDIS_PORT=6379
CRASH_DIR=$(mktemp -d)/crash
FAIL=0

# ---- 依赖服务：自签证书 + redis-server + mock 五合一 ----
CERT=$(mktemp -d)/server
openssl req -x509 -newkey rsa:2048 -keyout "$CERT.key" -out "$CERT.crt" \
  -days 1 -nodes -subj "/CN=localhost" 2>/dev/null
redis-server --port $REDIS_PORT --daemonize no --save '' > /dev/null 2>&1 & PIDS+=($!)
./$BUILD/tools/mock_services 50051 > /dev/null 2>&1 & PIDS+=($!)
sleep 0.8

start_gw() { # $1..n = 额外参数
  ./$BUILD/mediator --port=$WS_PORT --cert="$CERT.crt" --key="$CERT.key" \
    --backend=$BACKEND --redis-port=$REDIS_PORT --metrics-port=$METRICS_PORT \
    --crash-dir="$CRASH_DIR" "$@" > /dev/null 2>&1 & PIDS+=($!)
  sleep 0.8
}
stop_gw() { kill "${PIDS[-1]}" 2>/dev/null; wait "${PIDS[-1]}" 2>/dev/null; unset 'PIDS[-1]'; sleep 0.3; }

# ---- 场景1：真实 HS256 签名 ----
echo "=== scenario: real HS256 jwt ==="
start_gw
redis-cli -p $REDIS_PORT FLUSHALL > /dev/null
./$BUILD/tools/e2e_client $WS_PORT @sign || FAIL=1
# Redis 断言：在线键已写入
ONLINE=$(redis-cli -p $REDIS_PORT GET online:user-1)
if [ "$ONLINE" = "gw-local" ]; then echo "[PASS] redis online:user-1 = $ONLINE"; else
  echo "[FAIL] redis online:user-1 (got: '$ONLINE')"; FAIL=1; fi
# /metrics 断言：连接数/clip 指标存在
M=$(curl -s http://127.0.0.1:$METRICS_PORT/metrics)
echo "$M" | grep -q "clip_sent_total" && echo "[PASS] /metrics clip_sent_total" \
  || { echo "[FAIL] /metrics clip_sent_total"; FAIL=1; }
echo "$M" | grep -q "grpc_call_duration_ms" && echo "[PASS] /metrics grpc_call_duration_ms" \
  || { echo "[FAIL] /metrics grpc_call_duration_ms"; FAIL=1; }
stop_gw

# ---- 场景2：调试后门 token ----
echo "=== scenario: debug token ==="
start_gw --allow-debug-token=true
./$BUILD/tools/e2e_client $WS_PORT debug:user-1 || FAIL=1
stop_gw

# ---- 场景3：wasm 认证 ----
echo "=== scenario: wasm auth ==="
start_gw --auth-provider=wasm:$BUILD/tests/wasm/auth_allow.wasm \
  --observers=counter:$BUILD/tests/wasm/msg_counter.wasm
./$BUILD/tools/e2e_client $WS_PORT wasm-ok --expect-reject || FAIL=1
./$BUILD/tools/e2e_client $WS_PORT wasm-ok || FAIL=1
stop_gw

# ---- 场景4：崩溃转储 ----
echo "=== scenario: crash dump ==="
start_gw
GW_PID="${PIDS[-1]}"
kill -SEGV "$GW_PID" 2>/dev/null
sleep 0.5
unset 'PIDS[-1]'
if ls "$CRASH_DIR"/crash_*.txt > /dev/null 2>&1; then
  echo "[PASS] crash dump generated: $(ls "$CRASH_DIR" | head -2 | tr '\n' ' ')"
else
  echo "[FAIL] crash dump not generated"; FAIL=1
fi

if [ $FAIL -eq 0 ]; then echo "ALL E2E SCENARIOS PASSED"; else echo "E2E FAILED"; fi
exit $FAIL
