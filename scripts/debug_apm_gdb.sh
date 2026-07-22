#!/usr/bin/env bash
# gdb 定位 webrtc ProcessStream -11 来源
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
gdb -batch \
  -ex 'break webrtc::AudioProcessingImpl::ProcessStream(webrtc::AudioFrame*)' \
  -ex 'run --port=9443 --cert='"$D"'/s.crt --key='"$D"'/s.key --backend=127.0.0.1:50051 --redis-port=6379 --protocol=convai.v1:'"$B"'/tests/wasm/convai_proto.wasm --protocol-key=goldie-dev-key-2026' \
  -ex 'finish' -ex 'finish' -ex 'bt 4' \
  --args ./$B/mediator &
GDBPID=$!
sleep 3
timeout 30 ./$B/tools/convai_sim 9443 goldie-dev-key-2026 > /dev/null 2>&1 || true
sleep 1
kill $GDBPID 2>/dev/null
pkill -f "$B/mediator" 2>/dev/null; pkill mock_services 2>/dev/null; pkill redis-server 2>/dev/null
