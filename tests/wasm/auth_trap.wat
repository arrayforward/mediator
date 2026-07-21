;; auth_trap.wat — 测试用认证扩展：永远触发 trap（unreachable）
;; 用于验证 fail-closed：trap → 拒绝 + deny_reason="auth_internal"
(module
  (memory (export "memory") 1)
  (func (export "auth_verify") (param $ptr i32) (param $len i32) (result i32)
    unreachable))
