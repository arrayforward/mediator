#!/usr/bin/env bash
# ============================================================================
# run_android_interop.sh — Android 模拟器联调编排
#
# 拓扑：
#   Android 模拟器 --ws://10.0.2.2:9000--> [WSL localhostForwarding]
#   socat(9000→TLS 9443) --> mediator(WSS, convai.v1 wasm 插件) --> mock 五合一
#
# 10.0.2.2 是模拟器到宿主机 loopback 的别名；WSL2 默认 localhostForwarding
# 会把 Windows:9000 转发进 WSL:9000。
# ============================================================================
set -u
cd "$(dirname "$0")/.."
B=build-wsl
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

CERT=$(mktemp -d)/server
openssl req -x509 -newkey rsa:2048 -keyout "$CERT.key" -out "$CERT.crt" \
  -days 1 -nodes -subj "/CN=localhost" 2>/dev/null
redis-server --port 6379 --daemonize no --save '' >/dev/null 2>&1 & PIDS+=($!)
./$B/tools/mock_services 50051 >/dev/null 2>&1 & PIDS+=($!)
sleep 0.8
./$B/mediator --port=9443 --cert="$CERT.crt" --key="$CERT.key" \
  --backend=127.0.0.1:50051 --redis-port=6379 \
  --protocol=convai.v1:$B/tests/wasm/convai_proto.wasm \
  --protocol-key=goldie-dev-key-2026 --log-level=info >/tmp/gw_android.log 2>&1 & PIDS+=($!)
sleep 1
# 明文中继：模拟器(明文 ws) ↔ 网关(WSS)
socat TCP-LISTEN:9000,fork,reuseaddr,bind=0.0.0.0 OPENSSL:127.0.0.1:9443,verify=0 & PIDS+=($!)
sleep 0.5
echo "interop stack up: mock:50051 mediator:9443(wss) relay:9000(ws)"
echo "android app should use: ws://10.0.2.2:9000  product_key=goldie-dev-key-2026"
wait
