#!/usr/bin/env bash
# 诊断 restate/answer 偶发失败：保留网关与 mock 日志
set -u
cd "$(dirname "$0")/.."
B=build-wsl
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
sleep 0.3
D=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -keyout "$D/s.key" -out "$D/s.crt" -days 1 -nodes -subj /CN=localhost 2>/dev/null
redis-server --port 6379 --save '' >"$D/redis.log" 2>&1 &
./$B/tools/mock_services 50051 >"$D/mock.log" 2>&1 &
sleep 0.8
./$B/mediator --port=9443 --cert="$D/s.crt" --key="$D/s.key" --backend=127.0.0.1:50051 \
  --otel-endpoint=127.0.0.1:50051 --otel-interval-s=1 --crash-dir="$D/crash" \
  --allow-debug-token=true --log-level=debug >"$D/gw.log" 2>&1 &
sleep 1
timeout 60 ./$B/tools/e2e_client 9443 debug:user-1
echo "rc=$?"
echo "=== gw.log tail ==="
tail -40 "$D/gw.log"
echo "=== mock.log tail ==="
tail -15 "$D/mock.log"
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
