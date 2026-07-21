#!/usr/bin/env bash
# 符号化验证：让网关崩溃 → symbolize.sh 还原堆栈行号
set -u
cd "$(dirname "$0")/.."
D=$(mktemp -d)
./build-wsl/mediator --port=9444 --cert=nope --key=nope --crash-dir="$D" >/dev/null 2>&1 &
P=$!
sleep 0.7
kill -SEGV $P
sleep 0.5
DUMP=$(ls "$D"/crash_*.txt | head -1)
echo "=== crash dump head ==="
head -5 "$DUMP"
echo "=== symbolized ==="
bash scripts/symbolize.sh build-wsl/mediator "$DUMP" | head -8
