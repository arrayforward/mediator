#!/usr/bin/env bash
# 手工分步调试 E2E：启动 mock + 网关，跑单场景客户端，保留日志
set -u
cd "$(dirname "$0")/.."
B=build-wsl
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null
sleep 0.3

D=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -keyout "$D/s.key" -out "$D/s.crt" -days 1 -nodes -subj /CN=localhost 2>/dev/null

./$B/tools/mock_services 50051 >"$D/mock.log" 2>&1 &
MOCK=$!
sleep 0.8
./$B/mediator --port=9443 --cert="$D/s.crt" --key="$D/s.key" --log-level=debug >"$D/gw.log" 2>&1 &
GW=$!
sleep 1.5
echo "=== gateway log (startup) ==="
cat "$D/gw.log"

timeout 40 ./$B/tools/e2e_client 9443 'h.{"uid":"user-1"}.sig'
RC=$?
echo "=== client rc=$RC ==="
echo "=== gateway log (tail) ==="
tail -30 "$D/gw.log"
echo "=== mock log (tail) ==="
tail -10 "$D/mock.log"
kill $GW $MOCK 2>/dev/null
exit $RC
