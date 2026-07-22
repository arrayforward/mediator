#!/usr/bin/env bash
# convai E2E 调试：保留网关日志
set -u
cd "$(dirname "$0")/.."
B=build-wsl
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
sleep 0.3
D=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -keyout "$D/s.key" -out "$D/s.crt" -days 1 -nodes -subj /CN=localhost 2>/dev/null
redis-server --port 6379 --save '' >/dev/null 2>&1 &
./$B/tools/mock_services 50051 >"$D/mock.log" 2>&1 &
sleep 0.8
./$B/mediator --port=9443 --cert="$D/s.crt" --key="$D/s.key" --backend=127.0.0.1:50051 \
  --redis-port=6379 \
  --protocol=convai.v1:$B/tests/wasm/convai_proto.wasm \
  --protocol-key=goldie-dev-key-2026 \
  --log-level=debug >"$D/gw.log" 2>&1 &
sleep 1
timeout 60 ./$B/tools/convai_sim 9443 goldie-dev-key-2026
echo "rc=$?"
echo "=== gw.log tail ==="
tail -30 "$D/gw.log"
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
