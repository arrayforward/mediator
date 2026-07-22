#!/usr/bin/env bash
# 复跑 gdb 调试并统计 -11 出现时机
set -u
cd "$(dirname "$0")/.."
B=build-wsl
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
sleep 0.3
D=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -keyout "$D/s.key" -out "$D/s.crt" -days 1 -nodes -subj /CN=localhost 2>/dev/null
redis-server --port 6379 --save '' >/dev/null 2>&1 &
./$B/tools/mock_services 50051 >/dev/null 2>&1 &
sleep 0.8
./$B/mediator --port=9443 --cert="$D/s.crt" --key="$D/s.key" --backend=127.0.0.1:50051 \
  --protocol=convai.v1:$B/tests/wasm/convai_proto.wasm --protocol-key=goldie-dev-key-2026 \
  --log-level=debug > "$D/gw.log" 2>&1 &
sleep 1
timeout 25 ./$B/tools/convai_sim 9443 goldie-dev-key-2026 > "$D/sim.log" 2>&1
echo "== sim:"; grep -E 'PASS|FAIL' "$D/sim.log" | head -20
echo "== -11 count:"; grep -c 'err=-11' "$D/gw.log" || true
echo "== -11 context:"; grep -B2 -A2 'err=-11' "$D/gw.log" | head -12
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
