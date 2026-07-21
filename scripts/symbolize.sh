#!/usr/bin/env bash
# ============================================================================
# symbolize.sh — 崩溃转储符号化（设计文档 §7B.3）
#
# 用法：scripts/symbolize.sh <mediator二进制> <crash_xxx.txt>
# 从转储的 backtrace 中提取地址，用 addr2line 还原 文件:行号。
# RelWithDebInfo 构建自带调试符号；strip 后的线上包用 mediator.debug：
#   cmake --build build-wsl --target symbols
# ============================================================================
set -u
BIN=${1:?usage: symbolize.sh <binary> <crash.txt>}
DUMP=${2:?crash.txt}
[ -f "$BIN.debug" ] && BIN="$BIN.debug"

# PIE 二进制：转储中 "module(+0xOFFSET)" 的偏移才是 addr2line 的输入；
# 绝对地址受 ASLR 影响不可用。仅符号化属于本二进制的帧。
BIN_BASE=$(basename "$BIN" .debug)
grep -oE "$BIN_BASE\(\+0x[0-9a-f]+\)" "$DUMP" | grep -oE '0x[0-9a-f]+' | while read -r addr; do
  echo "$addr  $(addr2line -e "$BIN" -f -C "$addr" | tr '\n' ' ')"
done
