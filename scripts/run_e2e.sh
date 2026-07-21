#!/usr/bin/env bash
# ============================================================================
# run_e2e.sh — 端到端测试编排（设计文档 §9.3）
#
# 场景1（内置认证）：mock 五合一 + 网关(builtin) → e2e_client 全链路断言
# 场景2（wasm 认证）：网关 --auth-provider=wasm:e2e_auth.wasm →
#   e2e_client 错误 token 被拒 + "wasm-ok" 通过全链路（§7.3.3）
# ============================================================================
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD_DIR:-build-wsl}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

WS_PORT=9443
BACKEND=127.0.0.1:50051

# ---- 自签证书 ----
CERT=$(mktemp -d)/server
openssl req -x509 -newkey rsa:2048 -keyout "$CERT.key" -out "$CERT.crt" \
  -days 1 -nodes -subj "/CN=localhost" 2>/dev/null

# ---- 启动 mock 五合一 ----
./$BUILD/tools/mock_services 50051 & PIDS+=($!)
sleep 0.5

run_scenario() {
  local name=$1; shift
  local token=$1; shift
  echo "=== scenario: $name ==="
  ./$BUILD/mediator --port=$WS_PORT --cert="$CERT.crt" --key="$CERT.key" \
    --backend=$BACKEND "$@" & PIDS+=($!)
  sleep 0.8
  ./$BUILD/tools/e2e_client $WS_PORT "$token"
  local rc=$?
  kill "${PIDS[-1]}" 2>/dev/null; wait "${PIDS[-1]}" 2>/dev/null
  unset 'PIDS[-1]'
  return $rc
}

FAIL=0

# 场景1：内置 JWT 认证（stub：h.{payload}.sig 结构，uid 取自 payload）
run_scenario "builtin jwt auth" 'h.{"uid":"user-1"}.sig' || FAIL=1

# 场景2a：wasm 认证——错误 token 必须被拒
echo "=== scenario: wasm auth reject ==="
./$BUILD/mediator --port=$WS_PORT --cert="$CERT.crt" --key="$CERT.key" \
  --backend=$BACKEND --auth-provider=wasm:$BUILD/tests/wasm/auth_allow.wasm & PIDS+=($!)
sleep 0.8
./$BUILD/tools/e2e_client $WS_PORT wasm-ok --expect-reject || FAIL=1
# 场景2b：wasm 认证——wasm-ok 放行走全链路
./$BUILD/tools/e2e_client $WS_PORT wasm-ok || FAIL=1
kill "${PIDS[-1]}" 2>/dev/null; wait "${PIDS[-1]}" 2>/dev/null; unset 'PIDS[-1]'

if [ $FAIL -eq 0 ]; then echo "ALL E2E SCENARIOS PASSED"; else echo "E2E FAILED"; fi
exit $FAIL
