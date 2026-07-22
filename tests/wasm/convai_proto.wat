;; ============================================================================
;; convai_proto.wat — convai.v1 协议完整实现（wasm 插件，核心代码零协议）
;;
;; 协议全部内容在本模块：
;;   - 文本信封 {"type","seq","ts","body"} 的完整构造（含 itoa seq/ts）
;;   - 二进制 13B 音频头 Op(1)+Seq(4BE)+Ts(8BE) 的解析与构造
;;   - AudioOp 语义：0x10帧/0x11Start/0x12End/0x13Cancel
;;   - hello 设备 Key 鉴权、状态机、function_call 映射
;;
;; 宿主仅提供协议无关原语（env.*）：
;;   host_send_text(ptr,len)        裸文本帧
;;   host_send_binary(ptr,len)      裸二进制帧
;;   host_inject(type,flags,ptr,len) 注引擎消息
;;   host_json_get(jp,jl,kp,kl,out) JSON 取字段 → 写内存
;;   host_resample_g711(in,in_len,from,to,out) 泛型 G.711A 重采样
;;   host_bind_session / host_close / host_feed_watermark / host_uplink_asr
;;   host_now_ms() -> i64           时间戳
;;
;; 内存布局：
;;   [0,256)     json_get 输出（0:product_key, 64:uid, 512:type）
;;   [104]       信封 seq 计数    [108] 音频帧 seq 计数    [112] itoa 缓冲(32B)
;;   [256,2048)  JSON/帧构造区
;;   [2048,...)  常量模板（NUL 结尾）
;;   [8192,...)  宿主入站载荷      [12288,...) 重采样输出    [16384,...) 帧输出
;; ============================================================================
(module
  (import "env" "host_send_text"    (func $st (param i32 i32) (result i32)))
  (import "env" "host_send_binary"  (func $sb (param i32 i32) (result i32)))
  (import "env" "host_inject"       (func $inj (param i32 i32 i32 i32) (result i32)))
  (import "env" "host_json_get"     (func $jg (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "host_resample_g711" (func $rs (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "host_bind_session" (func $bind (param i32 i32) (result i32)))
  (import "env" "host_close"        (func $close (param i32) (result i32)))
  (import "env" "host_uplink_asr"   (func $uasr (param i32 i32 i32) (result i32)))
  (import "env" "host_feed_watermark" (func $wm (param i32 i32) (result i32)))
  (import "env" "host_now_ms"       (func $now (result i64)))

  (memory (export "memory") 1)

  ;; ---- 常量模板（NUL 结尾）----
  (data (i32.const 2048) "hello\00")
  (data (i32.const 2056) "config_update\00")
  (data (i32.const 2072) "function_call_output\00")
  (data (i32.const 2096) "bye\00")
  (data (i32.const 2104) "type\00")
  (data (i32.const 2112) "product_key\00")
  (data (i32.const 2124) "agent_id\00")
  (data (i32.const 2136) "android-client\00")
  (data (i32.const 2152) "hello_ack\00")
  (data (i32.const 2164) "hello_err\00")
  (data (i32.const 2176) "event\00")
  (data (i32.const 2184) "status\00")
  (data (i32.const 2192) "text\00")
  (data (i32.const 2200) "function_call\00")
  (data (i32.const 2216) "{\"code\":4401,\"message\":\"auth failed\"}\00")
  (data (i32.const 2264) "{\"event\":\"connected\",\"details\":\"\"}\00")
  (data (i32.const 2312) "{\"status\":\"listening\"}\00")
  (data (i32.const 2344) "{\"status\":\"thinking\"}\00")
  (data (i32.const 2376) "{\"type\":\"response.function_call_arguments.done\",\"calls\":[{\"call_id\":\"fc-1\",\"name\":\"emotion\",\"arguments\":\"{\\\"emotion\\\":\\\"happy\\\"}\"}]}\00")
  (data (i32.const 2600) "{\"session_id\":\"\00")
  (data (i32.const 2616) "\",\"server_time\":0,\"audio_config\":{\"frame_ms\":20,\"codec\":\"g711a\"}}\00")
  (data (i32.const 2700) "{\"text\":\"\00")
  (data (i32.const 2712) "\"}\00")

  ;; ================= 基础工具 =================

  (func $slen (param $p i32) (result i32)
    (local $n i32)
    (loop $c
      (if (i32.ne (i32.load8_u (i32.add (local.get $p) (local.get $n))) (i32.const 0))
        (then (local.set $n (i32.add (local.get $n) (i32.const 1))) (br $c))))
    (local.get $n))

  (func $eq (param $a i32) (param $b i32) (param $n i32) (result i32)
    (local $i i32)
    (block $ne
      (loop $c
        (if (i32.ge_u (local.get $i) (local.get $n)) (then (return (i32.const 1))))
        (if (i32.ne (i32.load8_u (i32.add (local.get $a) (local.get $i)))
                    (i32.load8_u (i32.add (local.get $b) (local.get $i))))
          (then (br $ne)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $c)))
    (i32.const 0))

  (func $eqs (param $val i32) (param $vlen i32) (param $tpl i32) (result i32)
    (if (i32.ne (local.get $vlen) (call $slen (local.get $tpl)))
      (then (return (i32.const 0))))
    (call $eq (local.get $val) (local.get $tpl) (local.get $vlen)))

  (func $cpy (param $dst i32) (param $src i32) (param $n i32) (result i32)
    (local $i i32)
    (loop $c
      (if (i32.lt_u (local.get $i) (local.get $n))
        (then
          (i32.store8 (i32.add (local.get $dst) (local.get $i))
                      (i32.load8_u (i32.add (local.get $src) (local.get $i))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $c))))
    (i32.add (local.get $dst) (local.get $n)))

  ;; itoa32：数字写到 [112..]，长度写到 mem[148]（wasm3 不支持多返回值）
  (func $itoa32 (param $v i32)
    (local $p i32)
    (local $start i32)
    (local $len i32)
    (local.set $p (i32.const 143)) ;; 从缓冲尾部倒写
    (if (i32.eqz (local.get $v))
      (then
        (i32.store8 (i32.const 112) (i32.const 48))
        (i32.store (i32.const 148) (i32.const 1))
        (return)))
    (block $done
      (loop $d
        (if (i32.eqz (local.get $v)) (then (br $done)))
        (i32.store8 (local.get $p)
          (i32.add (i32.const 48) (i32.rem_u (local.get $v) (i32.const 10))))
        (local.set $v (i32.div_u (local.get $v) (i32.const 10)))
        (local.set $p (i32.sub (local.get $p) (i32.const 1)))
        (br $d)))
    (local.set $start (i32.add (local.get $p) (i32.const 1)))
    (local.set $len (i32.sub (i32.const 143) (local.get $p)))
    (drop (call $cpy (i32.const 112) (local.get $start) (local.get $len)))
    (i32.store (i32.const 148) (local.get $len)))

  ;; itoa64：同上（i64 版，ts 用）
  (func $itoa64 (param $v i64)
    (local $p i32)
    (local $start i32)
    (local $len i32)
    (local.set $p (i32.const 143))
    (if (i64.eqz (local.get $v))
      (then
        (i32.store8 (i32.const 112) (i32.const 48))
        (i32.store (i32.const 148) (i32.const 1))
        (return)))
    (block $done
      (loop $d
        (if (i64.eqz (local.get $v)) (then (br $done)))
        (i32.store8 (local.get $p)
          (i32.add (i32.const 48)
            (i32.wrap_i64 (i64.rem_u (local.get $v) (i64.const 10)))))
        (local.set $v (i64.div_u (local.get $v) (i64.const 10)))
        (local.set $p (i32.sub (local.get $p) (i32.const 1)))
        (br $d)))
    (local.set $start (i32.add (local.get $p) (i32.const 1)))
    (local.set $len (i32.sub (i32.const 143) (local.get $p)))
    (drop (call $cpy (i32.const 112) (local.get $start) (local.get $len)))
    (i32.store (i32.const 148) (local.get $len)))

  ;; ================= 协议构造 =================

  ;; 发文本信封：{"type":"<tpl>","seq":N,"ts":M,"body":<body>}
  (func $send_env (param $tpl i32) (param $body i32) (param $blen i32)
    (local $end i32)
    (local $nl i32)
    (local $seq i32)
    (local.set $end (i32.const 256))
    (i32.store8 (local.get $end) (i32.const 123)) ;; {
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))  ;; "
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (local.set $end (call $cpy (local.get $end) (i32.const 2104) (i32.const 4))) ;; type
    (local.set $end (i32.add (local.get $end) (i32.const 0)))
    ;; ":" 引号与冒号
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 58)) ;; :
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (local.set $end (call $cpy (local.get $end) (local.get $tpl) (call $slen (local.get $tpl))))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    ;; ,"seq":
    (i32.store8 (local.get $end) (i32.const 44))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 115)) ;; s
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 101)) ;; e
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 113)) ;; q
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 58))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    ;; seq 数字
    (local.set $seq (i32.load (i32.const 104)))
    (i32.store (i32.const 104) (i32.add (local.get $seq) (i32.const 1)))
    (call $itoa32 (local.get $seq))
    (local.set $nl (i32.load (i32.const 148))) ;; itoa 长度约定在 148
    (local.set $end (call $cpy (local.get $end) (i32.const 112) (local.get $nl)))
    ;; ,"ts":
    (i32.store8 (local.get $end) (i32.const 44))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 116)) ;; t
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 115)) ;; s
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 58))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (call $itoa64 (call $now))
    (local.set $nl (i32.load (i32.const 148)))
    (local.set $end (call $cpy (local.get $end) (i32.const 112) (local.get $nl)))
    ;; ,"body":
    (i32.store8 (local.get $end) (i32.const 44))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 98)) ;; b
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 111)) ;; o
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 100)) ;; d
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 121)) ;; y
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 34))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (i32.store8 (local.get $end) (i32.const 58))
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (local.set $end (call $cpy (local.get $end) (local.get $body) (local.get $blen)))
    (i32.store8 (local.get $end) (i32.const 125)) ;; }
    (local.set $end (i32.add (local.get $end) (i32.const 1)))
    (drop (call $st (i32.const 256) (i32.sub (local.get $end) (i32.const 256)))))

  ;; 发二进制音频帧：13B 头 op/seq/ts + payload
  (func $send_audio (param $op i32) (param $p i32) (param $n i32)
    (local $seq i32)
    (local $ts i64)
    (local $out i32)
    (local $i i32)
    (local.set $out (i32.const 16384))
    (local.set $seq (i32.load (i32.const 108)))
    (i32.store (i32.const 108) (i32.add (local.get $seq) (i32.const 1)))
    (local.set $ts (call $now))
    (i32.store8 (local.get $out) (local.get $op))
    ;; seq 4B BE
    (i32.store8 (i32.add (local.get $out) (i32.const 1))
                (i32.and (i32.shr_u (local.get $seq) (i32.const 24)) (i32.const 255)))
    (i32.store8 (i32.add (local.get $out) (i32.const 2))
                (i32.and (i32.shr_u (local.get $seq) (i32.const 16)) (i32.const 255)))
    (i32.store8 (i32.add (local.get $out) (i32.const 3))
                (i32.and (i32.shr_u (local.get $seq) (i32.const 8)) (i32.const 255)))
    (i32.store8 (i32.add (local.get $out) (i32.const 4))
                (i32.and (local.get $seq) (i32.const 255)))
    ;; ts 8B BE
    (local.set $i (i32.const 0))
    (loop $t
      (if (i32.lt_s (local.get $i) (i32.const 8))
        (then
          (i32.store8 (i32.add (local.get $out) (i32.add (i32.const 5) (local.get $i)))
            (i32.wrap_i64 (i64.and (i64.shr_u (local.get $ts)
              (i64.extend_i32_u (i32.mul (i32.sub (i32.const 7) (local.get $i)) (i32.const 8))))
              (i64.const 255))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $t))))
    (drop (call $cpy (i32.add (local.get $out) (i32.const 13)) (local.get $p) (local.get $n)))
    (drop (call $sb (local.get $out) (i32.add (i32.const 13) (local.get $n)))))

  ;; ================= 帧事件 =================

  (func (export "on_ws_text") (param $ptr i32) (param $len i32)
    (local $tl i32)
    (local.set $tl (call $jg (local.get $ptr) (local.get $len)
                             (i32.const 2104) (i32.const 4) (i32.const 512)))
    (if (i32.lt_s (local.get $tl) (i32.const 0)) (then return))
    (if (call $eqs (i32.const 512) (local.get $tl) (i32.const 2048))
      (then (call $hello (local.get $ptr) (local.get $len)) (return)))
    (if (call $eqs (i32.const 512) (local.get $tl) (i32.const 2056))
      (then (drop (call $inj (i32.const 1) (i32.const 0)
                             (local.get $ptr) (local.get $len))) (return)))
    (if (call $eqs (i32.const 512) (local.get $tl) (i32.const 2096))
      (then (drop (call $inj (i32.const 3) (i32.const 0) (i32.const 0) (i32.const 0)))
            (return)))
  )

  (func $hello (param $ptr i32) (param $len i32)
    (local $kl i32)
    (local $ul i32)
    (local $end i32)
    (local.set $kl (call $jg (local.get $ptr) (local.get $len)
                             (i32.const 2112) (i32.const 11) (i32.const 0)))
    ;; 设备 Key：宿主注入 [1024:u32 len][1028:bytes]
    (if (i32.or (i32.ne (local.get $kl) (i32.load (i32.const 1024)))
                (i32.eqz (call $eq (i32.const 0) (i32.const 1028) (local.get $kl))))
      (then
        (call $send_env (i32.const 2164) (i32.const 2216) (call $slen (i32.const 2216)))
        (drop (call $close (i32.const 4401)))
        (return)))
    (local.set $ul (call $jg (local.get $ptr) (local.get $len)
                             (i32.const 2124) (i32.const 8) (i32.const 64)))
    (if (i32.lt_s (local.get $ul) (i32.const 0))
      (then
        (drop (call $cpy (i32.const 64) (i32.const 2136) (call $slen (i32.const 2136))))
        (local.set $ul (call $slen (i32.const 2136)))))
    (drop (call $bind (i32.const 64) (local.get $ul)))
    ;; hello_ack body
    (local.set $end (call $cpy (i32.const 1536) (i32.const 2600) (call $slen (i32.const 2600))))
    (local.set $end (call $cpy (local.get $end) (i32.const 64) (local.get $ul)))
    (local.set $end (call $cpy (local.get $end) (i32.const 2616) (call $slen (i32.const 2616))))
    (call $send_env (i32.const 2152) (i32.const 1536) (i32.sub (local.get $end) (i32.const 1536)))
    (call $send_env (i32.const 2176) (i32.const 2264) (call $slen (i32.const 2264)))
    (call $send_env (i32.const 2184) (i32.const 2312) (call $slen (i32.const 2312)))
  )

  ;; 裸二进制帧：自行解析 13B 头（协议在插件内）
  (func (export "on_ws_binary") (param $ptr i32) (param $len i32)
    (local $op i32)
    (local $pl i32)
    (if (i32.lt_s (local.get $len) (i32.const 13)) (then (return)))
    (local.set $op (i32.load8_u (local.get $ptr)))
    (local.set $pl (i32.add (local.get $ptr) (i32.const 13)))
    (local.set $len (i32.sub (local.get $len) (i32.const 13)))
    ;; mem[100]：标定状态
    (if (i32.eq (local.get $op) (i32.const 0x10))
      (then
        (if (i32.eqz (i32.load8_u (i32.const 100)))
          (then
            (if (i32.eq (call $wm (local.get $pl) (local.get $len)) (i32.const 1))
              (then (i32.store8 (i32.const 100) (i32.const 1)))))
          (else
            (drop (call $uasr (i32.const 4) (local.get $pl) (local.get $len)))))
        (return)))
    (if (i32.eq (local.get $op) (i32.const 0x11))
      (then (drop (call $uasr (i32.const 4) (i32.const 0) (i32.const 0))) (return)))
    (if (i32.eq (local.get $op) (i32.const 0x12))
      (then
        (if (i32.load8_u (i32.const 100))
          (then (drop (call $uasr (i32.const 6) (local.get $pl) (local.get $len)))))
        (return)))
    ;; Cancel(0x13)：忽略
  )

  ;; 引擎下行 clip（16k G.711A）→ 8k → 0x11 + 0x10×N + 0x12
  (func (export "on_outbound_clip") (param $clip i32) (param $ptr i32) (param $len i32)
    (local $n i32)
    (local $i i32)
    (local $c i32)
    (local.set $n (call $rs (local.get $ptr) (local.get $len)
                            (i32.const 16000) (i32.const 8000) (i32.const 12288)))
    (if (i32.le_s (local.get $n) (i32.const 0)) (then return))
    (call $send_audio (i32.const 0x11) (i32.const 0) (i32.const 0))
    (block $done
      (loop $emit
        (if (i32.ge_s (local.get $i) (local.get $n)) (then (br $done)))
        (local.set $c (i32.sub (local.get $n) (local.get $i)))
        (if (i32.gt_s (local.get $c) (i32.const 160))
          (then (local.set $c (i32.const 160))))
        (call $send_audio (i32.const 0x10)
                          (i32.add (i32.const 12288) (local.get $i)) (local.get $c))
        (local.set $i (i32.add (local.get $i) (local.get $c)))
        (br $emit)))
    (call $send_audio (i32.const 0x12) (i32.const 0) (i32.const 0))
  )

  ;; 引擎下行文本：前缀 "ack:" → function_call
  (func (export "on_outbound_text") (param $ptr i32) (param $len i32)
    (local $r i32)
    (if (i32.lt_s (local.get $len) (i32.const 4)) (then (return)))
    (local.set $r (i32.eq (i32.load8_u (local.get $ptr)) (i32.const 97)))
    (local.set $r (i32.and (local.get $r)
        (i32.eq (i32.load8_u (i32.add (local.get $ptr) (i32.const 1))) (i32.const 99))))
    (local.set $r (i32.and (local.get $r)
        (i32.eq (i32.load8_u (i32.add (local.get $ptr) (i32.const 2))) (i32.const 107))))
    (local.set $r (i32.and (local.get $r)
        (i32.eq (i32.load8_u (i32.add (local.get $ptr) (i32.const 3))) (i32.const 58))))
    (if (local.get $r)
      (then (call $send_env (i32.const 2200) (i32.const 2376) (call $slen (i32.const 2376)))))
  )

  (func (export "on_llm_text") (param $ptr i32) (param $len i32)
    (local $end i32)
    (local.set $end (call $cpy (i32.const 1536) (i32.const 2700) (call $slen (i32.const 2700))))
    (local.set $end (call $cpy (local.get $end) (local.get $ptr) (local.get $len)))
    (local.set $end (call $cpy (local.get $end) (i32.const 2712) (call $slen (i32.const 2712))))
    (call $send_env (i32.const 2192) (i32.const 1536) (i32.sub (local.get $end) (i32.const 1536)))
  )

  (func (export "on_thinking")
    (call $send_env (i32.const 2184) (i32.const 2344) (call $slen (i32.const 2344))))
)
