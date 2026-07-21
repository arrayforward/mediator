;; auth_allow.wat — 测试用认证扩展：token == "wasm-ok" 时放行
;; ABI：导出 memory + auth_verify(ptr, len) -> i32 (1=允许 0=拒绝)
(module
  (memory (export "memory") 1)
  (func (export "auth_verify") (param $ptr i32) (param $len i32) (result i32)
    (local $i i32)
    ;; 期望 token = "wasm-ok"（7 字节），逐字节比较内存
    (if (i32.ne (local.get $len) (i32.const 7))
      (then (return (i32.const 0))))
    (block $mismatch
      (loop $check
        (if (i32.ge_u (local.get $i) (i32.const 7))
          (then (return (i32.const 1))))
        ;; 与常量串逐字节比较（常量按字符内联）
        (if (i32.ne
              (i32.load8_u (i32.add (local.get $ptr) (local.get $i)))
              (i32.load8_u (i32.add (i32.const 128) (local.get $i))))
          (then (br $mismatch)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $check)))
    (return (i32.const 0)))
  ;; 期望串 "wasm-ok" 放在内存偏移 128
  (data (i32.const 128) "wasm-ok"))
