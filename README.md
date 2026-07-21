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

C++20 · Boost.Beast (WSS) · gRPC (异步 CQ) · WebRTC APM · hiredis · jwt-cpp · Wasmtime · opentelemetry-cpp · Google Breakpad · GoogleTest · CMake + vcpkg (WSL2/Ubuntu)

## 构建与测试（WSL Ubuntu 22.04）

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config autoconf libtool
git clone https://github.com/microsoft/vcpkg && ./vcpkg/bootstrap-vcpkg.sh

cmake -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure        # 单元 + 心跳 + 会话测试
cmake --build build --target symbols              # 抽调试符号入符号仓库
```

端到端测试（mock 微服务 + 模拟端侧）：

```bash
./build/tools/mock_services/mock_services &        # ASR/LLM/TTS/Business/Memory 五合一
./build/mediator --auth-provider=builtin &
./build/tools/e2e_client/e2e_client                # 全链路: wav→A/B/C→控制指令→断线重连
```

wasm 认证扩展模式：

```bash
./build/mediator --auth-provider=wasm:e2e_auth     # 激活 wasm 认证扩展
```

## 工程结构

```
proto/    gRPC 接口定义 (asr/llm/tts/business/memory)
src/
  core/       Reactor/定时器/CSP Channel/虚拟时钟（框架层）
  engine/     心跳引擎/数据黑板/单层演进/ChangeSet
  net/        WSS 服务/JWT/gRPC 客户端/会话粘性
  audio/      APM 封装/水印标定/对齐重采样/G.711A/播放队列
  session/    会话上下文/在线注册/Redis/生命周期
  ext/        wasm 总线/动态加载管理/观察者/wasm 认证
  telemetry/  OTel 指标封装
  crash/      Breakpad/符号化/watchdog
tests/    GoogleTest 单元与心跳测试
tools/    mock_services（五合一 mock）、e2e_client（端侧模拟器）
```
