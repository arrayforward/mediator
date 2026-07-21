#!/usr/bin/env bash
# 连跑 N 次 E2E 观察稳定性
set -u
cd "$(dirname "$0")/.."
N=${1:-5}
for i in $(seq 1 "$N"); do
  echo "--- run $i ---"
  timeout 300 bash scripts/run_e2e.sh 2>/dev/null | grep -E "FAIL|SCENARIOS"
done
