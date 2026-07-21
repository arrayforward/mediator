#!/usr/bin/env bash
# 复现 run_e2e 场景1（@sign + redis + metrics-port + otlp）并留日志
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
  --redis-port=6379 --otel-endpoint=127.0.0.1:50051 --otel-interval-s=1 \
  --metrics-port=19099 --crash-dir="$D/crash" \
  --log-level=debug >"$D/gw.log" 2>&1 &
sleep 1
redis-cli -p 6379 FLUSHALL > /dev/null
timeout 60 ./$B/tools/e2e_client 9443 @sign
echo "rc=$?"
echo "=== gw.log (asr/llm/tts/ws_send) ==="
grep -E "asr|llm|tts|ws_send|error|warn" "$D/gw.log" | tail -25
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
