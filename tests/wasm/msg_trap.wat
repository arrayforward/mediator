;; msg_trap.wat — 观察者熔断测试模块：on_message 永远 trap
(module
  (memory (export "memory") 1)
  (func (export "on_message") (param $type i32) (param $ptr i32) (param $len i32)
    unreachable))
