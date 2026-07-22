#!/usr/bin/env bash
# ============================================================================
# run_e2e_convai.sh — convai.v1 协议端到端（GoldieSettingsAndroid 联调路径）
#
# mock 五合一 + 网关（--protocol=convai.v1:wasm插件 --protocol-key）+
# convai_sim（模拟 Android 客户端，含错误 Key 反例）
# ============================================================================
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD_DIR:-build-wsl}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

WS_PORT=9443
MOCK_PORT=${MOCK_PORT:-50051}
DEVICE_KEY=goldie-dev-key-2026
FAIL=0

CERT=$(mktemp -d)/server
openssl req -x509 -newkey rsa:2048 -keyout "$CERT.key" -out "$CERT.crt" \
  -days 1 -nodes -subj "/CN=localhost" 2>/dev/null
redis-server --port 6379 --daemonize no --save '' > /dev/null 2>&1 & PIDS+=($!)
./$BUILD/tools/mock_services $MOCK_PORT > /dev/null 2>&1 & PIDS+=($!)
sleep 0.8

./$BUILD/mediator --port=$WS_PORT --cert="$CERT.crt" --key="$CERT.key" \
  --backend=127.0.0.1:$MOCK_PORT --redis-port=6379 \
  --protocol=convai.v1:$BUILD/tests/wasm/convai_proto.wasm \
  --protocol-key=$DEVICE_KEY > /dev/null 2>&1 & PIDS+=($!)
sleep 1

echo "=== convai.v1 e2e (device key auth) ==="
timeout 120 ./$BUILD/tools/convai_sim $WS_PORT $DEVICE_KEY || FAIL=1

echo "=== convai.v1 wrong key rejected ==="
if timeout 30 ./$BUILD/tools/convai_sim $WS_PORT wrong-key 2>/dev/null; then
  echo "[FAIL] wrong device key accepted"; FAIL=1
else
  echo "[PASS] wrong device key rejected (hello_err + 4401)"
fi

if [ $FAIL -eq 0 ]; then echo "CONVAI E2E PASSED"; else echo "CONVAI E2E FAILED"; fi
exit $FAIL
