# mediator — AI 语音网关

接入 IoT 端侧 WebSocket 音频流，经回声消除（AEC3）/降噪/VAD 后，串联 ASR → LLM → TTS 的 gRPC 微服务链路，以 **A/B/C 三段式音频流水线** 将首音频延迟压缩到接近 0，为用户语音对话提供情绪安抚（A）→ 问题复述（B）→ 完整答案（C）的渐进式响应体验。

## 核心特性

- **反应式消息驱动架构**：四阶段（输入→传递→心跳批处理→副作用执行）+ 双轨驱动（消息驱动 + 单层数据演进），共享数据仅心跳线程单写，CSP 值语义消息队列，从根源消除死锁
- **音频流水线**：WebRTC APM（AEC3 回声消除 / NS 降噪 / VAD），G.711A 编码下发；**双音水印标定法**解决跨网络 render/capture 时钟漂移对齐
- **三段式响应**：说话中预生成安抚音频 A → 说完立即播放；复述 B、答案 C 并行生成按序下发，首包延迟 ≈ 0
- **安全**：WSS + JWT 验签；**可插拔 wasm 认证扩展**（启动参数 `--auth-provider=wasm:<name>` 激活，fail-closed）；Redis 防重登 + 会话代际防污染；3 分钟断线重连续聊；1MB 上下文淘汰
- **可扩展**：观察者模式消息总线 + wasm 扩展**动态加载/热更新**（心跳边界原子切换、双实例 draining、fuel/超时熔断）
- **可观测**：OpenTelemetry Metrics（OTLP + Prometheus 双导出），20+ 业务指标，慢任务告警，压力自适应保护
- **可调试**：Breakpad minidump + coredump 兜底、符号仓库（build-id 关联）、minidump_stackwalk / llvm-symbolizer 符号化、backward-cpp 活体堆栈
- **内建可测试性**：虚拟时钟、消息/黑板注入、单步心跳 `RunOnce()`、确定性重放；全量 C++ 单元测试 + 端到端测试工具

## 文档

- [docs/design.md](docs/design.md) — 详细设计文档（架构、线程模型、消息契约、AEC 水印标定、wasm 动态加载、OTel 指标、崩溃调试、测试计划）

## 技术栈

C++20 · Boost.Beast (WSS/TLS) · gRPC · WebRTC APM（待接） · **wasm3**（third_party 内置） · spdlog · jwt-cpp（待接） · opentelemetry-cpp（待接） · Google Breakpad（待接） · GoogleTest · CMake (WSL2/Ubuntu 22.04 系统包)

## 构建与测试（WSL Ubuntu 22.04）

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config \
  libgrpc++-dev protobuf-compiler-grpc libboost-dev libssl-dev libspdlog-dev wabt

cmake -B build-wsl -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-wsl
ctest --test-dir build-wsl --output-on-failure   # 52+ 单元测试（含 wasm 认证）
```

端到端测试（mock 五合一 gRPC + WSS 网关 + 模拟端侧，一条命令）：

```bash
bash scripts/run_e2e.sh
# 场景1: 内置认证全链路（水印回环标定→安抚/复述/答案音频顺序下发→控制ack→断线重连）
# 场景2: wasm 认证（错误 token 被拒 + wasm-ok 放行走全链路）
```

手动运行：

```bash
openssl req -x509 -newkey rsa:2048 -keyout s.key -out s.crt -days 1 -nodes -subj /CN=localhost
./build-wsl/tools/mock_services 50051 &                        # ASR/LLM/TTS/Business/Memory 五合一
./build-wsl/mediator --port=9443 --cert=s.crt --key=s.key \
  --auth-provider=builtin &                                    # 或 wasm:build-wsl/tests/wasm/auth_allow.wasm
./build-wsl/tools/e2e_client 9443 'h.{"uid":"user-1"}.sig'
```

## 工程结构

```
proto/    mediator.proto（ASR bidi / LLM / TTS / Business / Memory）
src/
  core/       时钟/CSP Channel/定时器/Reactor/线程池/spdlog 封装
  engine/     消息契约/数据黑板/心跳引擎（RunOnce + 单层演进）
  net/        WSS 服务器(Beast+OpenSSL)/gRPC 客户端/会话粘性
  audio/      G.711A/双音水印标定/延迟与重采样对齐
  session/    上下文淘汰(1MB→50%)
  ext/        AuthProvider 可插拔认证 + wasm3 宿主(动态加载/WasmAuth)
  gateway     装配器：心跳线程 + ChangeSet 执行器 + ASR 流管理
tests/    52+ GoogleTest 用例 + wasm/*.wat 认证测试模块
tools/    mock_services（五合一 mock gRPC）、e2e_client（WSS 模拟端侧）
scripts/  run_e2e.sh（E2E 编排：内置认证 + wasm 认证双场景）
third_party/wasm3/
```

## 当前状态

全部模块已落地，**75 单元测试 + 双协议 E2E 全绿**：

- **convai.v1 联调路径（GoldieSettingsAndroid）**：协议全部在 wasm 插件内（核心代码零协议），`--protocol=convai.v1:plugin.wasm --protocol-key=<设备Key>` 配置路由；设备 Key 鉴权、水印声纹首次录音可闻（后续免重标）、8k↔16k 边界重采样、控制指令过 mock gRPC、`bash scripts/run_e2e_convai.sh` 全消息流断言
- 四阶段心跳引擎 + 三段式音频流水线（安抚/复述/答案 + 场景化占位与复用）
- WSS(TLS) + **JWT HS256 真实验签** + 调试后门 token（`--allow-debug-token`）
- **wasm3** 认证扩展 + 观察者消息扩展 + **协议解析插件**（三类角色同一宿主机制）
- **WebRTC APM**（AECM/NS/VAD，每会话 actor 管线）+ 双音水印标定（8k/16k 双模板）
- **Redis**（hiredis）、**OTLP 指标推送** + **/metrics 抓取**、**崩溃转储 + 符号化**
- 原 JWT 路径回归：`bash scripts/run_e2e.sh`（stress 5/5）

偏差说明（AECM vs AEC3、Prometheus vs OTel SDK、自研转储 vs Breakpad）见 [docs/design.md](docs/design.md) §10A；convai 联调细节与联调缺陷记录见 §10B。

- 四阶段心跳引擎 + 三段式音频流水线（安抚/复述/答案 + 场景化占位与复用）
- WSS(TLS) + **JWT HS256 真实验签**（HMAC-SHA256+exp）+ 调试后门 token（`--allow-debug-token`，`debug:<uid>` 免签名，仅 E2E 调试用）
- **wasm3** 认证扩展（`--auth-provider=wasm:<path>`）+ wasm 观察者消息扩展（trap 熔断）
- **WebRTC APM**（AECM/NS/VAD，系统包 0.3.1）+ 双音水印标定（回环 E2E 真实检出）
- **Redis**（hiredis）：在线状态、上下文/占位音频持久化、断线重连恢复
- **指标**：OTel 语义 API + Prometheus `/metrics` 端点（`--metrics-port`）
- **崩溃转储**：signal handler + backtrace 转储（`--crash-dir`）、`symbols` target 抽调试符号、`scripts/symbolize.sh` 还原行号（PIE 偏移）

偏差说明（AECM vs AEC3、Prometheus vs OTel SDK、自研转储 vs Breakpad）见 [docs/design.md](docs/design.md) §10A。

## 启动参数

```
--port --cert --key            WSS 监听与 TLS 证书
--backend                      后端 gRPC 地址
--auth-provider=builtin|wasm:<path>
--jwt-secret                   HS256 验签密钥
--allow-debug-token=true       调试后门 token（生产严禁）
--redis-host --redis-port      Redis 持久化
--otel-endpoint                OTel Collector（OTLP gRPC 推送）
--metrics-port                 /metrics 抓取端点
--observers=name:path,...      wasm 观察者订阅
--protocol=subproto:plugin.wasm,...   协议路由（如 convai.v1:xxx.wasm）
--protocol-key                 协议插件配置槽（如设备 Key）
--crash-dir                    崩溃转储目录
--enable-apm=false             关闭 WebRTC APM
```
