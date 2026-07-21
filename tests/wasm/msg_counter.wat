;; msg_counter.wat — 观察者测试模块：on_message 每次调用计数 +1（mem[64]）
;; 导出 get_count 供宿主断言；msg_trap.wat 用于熔断测试
(module
  (memory (export "memory") 1)
  (func (export "on_message") (param $type i32) (param $ptr i32) (param $len i32)
    ;; mem[64] += 1
    (i32.store (i32.const 64)
      (i32.add (i32.load (i32.const 64)) (i32.const 1))))
  (func (export "get_count") (result i32)
    (i32.load (i32.const 64))))
