// ============================================================================
// wasm3_host.h — wasm3 沙箱宿主（设计文档 §7.2/§7.3，运行时选型 wasm3）
//
// 实现思路：
//   wasm3 是单文件解释器（无 JIT），体积小、启动快，适合网关注入式扩展。
//   每个扩展模块拥有独立 Runtime（内存/栈隔离），加载失败不影响宿主。
//
// 三类扩展角色（同一宿主机制）：
//   1. 认证提供者（§7.3）：导出 auth_verify(ptr,len)->i32
//   2. 消息观察者（§7.1）：导出 on_message(type,ptr,len)，只读+熔断
//   3. 协议插件（联调方案）：导出 on_ws_text/on_ws_audio/on_outbound_*，
//      宿主经 LinkHostFunction 注入 env.* 原语（发送/编解码/消息注入），
//      wasm 内实现 convai.v1 协议状态机（观察者模式解析协议）
//
// 与 Wasmtime 方案差异：wasm3 无 fuel 概念，资源限制通过
//   runtime 内存上限 + 栈大小约束；调用超时由调用方线程隔离保证。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ext/auth_provider.h"

struct M3Environment;
struct M3Runtime;
struct M3Function;

namespace mediator::ext {

// 单个已加载的 wasm 模块实例
class Wasm3Module {
public:
    Wasm3Module();
    ~Wasm3Module();
    Wasm3Module(const Wasm3Module&) = delete;
    Wasm3Module& operator=(const Wasm3Module&) = delete;

    // 从字节流加载并编译；失败返回 false（错误信息经 LastError 读取）
    bool Load(const std::vector<uint8_t>& bytes);
    bool LoadFromFile(const std::string& path);

    // 认证 ABI：调用导出的 auth_verify；返回 <0 表示内部错误（fail-closed）
    int CallAuthVerify(const std::string& token);

    // 观察者 ABI：检查导出 + 调用 on_message(type, ptr, len)；返回 false=trap
    bool HasFunction(const char* name);
    bool CallOnMessage(int32_t msg_type, const std::vector<uint8_t>& payload,
                       uint32_t offset);
    // 读线性内存（测试断言用）
    bool ReadMemory(uint32_t offset, void* out, size_t len);

    // ---- 协议插件 ABI（宿主原语注入 + 通用导出调用）----
    // 链接宿主函数到模块 env 命名空间；sig 为 wasm3 签名（如 "v(ii)"）
    // fn 为 m3ApiRawFunction 风格的 C 函数
    bool LinkHostFunction(const char* name, const char* sig, const void* fn);
    // 写线性内存（宿主在调用导出前放置输入数据）
    bool WriteMemory(uint32_t offset, const void* data, size_t len);
    // 调用导出函数（字符串参数），成功返回 true 并带出 i32 结果
    bool CallExportI32(const char* name, const std::vector<std::string>& args,
                       int32_t* result = nullptr);
    // 取线性内存指针与大小（宿主原语实现用）
    uint8_t* Memory(uint32_t* size_out);

    const std::string& LastError() const { return m_lastError; }

    static constexpr uint32_t kTokenOffset = 1024; // token 写入线性内存的固定偏移
    static constexpr size_t kRuntimeMemSize = 64 * 1024;

private:
    M3Environment* m_env = nullptr;
    M3Runtime* m_rt = nullptr;
    M3Function* m_authVerify = nullptr;
    void* m_module = nullptr; // IM3Module（LinkHostFunction 用）
    std::string m_lastError;
    // wasm3 不拷贝模块字节（m3_ParseModule 仅引用），宿主必须持有到卸载
    std::vector<uint8_t> m_bytes;
};

// 动态加载管理器（§7.2）：按名称加载/卸载/热更新
// v1：同步加载 + 立即切换；双实例 draining 留待多线程心跳边界接入
class WasmModuleManager {
public:
    // 加载并激活；已存在同名模块则替换（热更新）。失败返回 false 且保留旧实例。
    bool Load(const std::string& name, const std::string& path);
    void Unload(const std::string& name);
    Wasm3Module* Find(const std::string& name);
    size_t Count() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Wasm3Module>> m_modules;
};

// wasm 认证提供者：fail-closed
class WasmAuth final : public AuthProvider {
public:
    explicit WasmAuth(Wasm3Module* mod) : m_mod(mod) {}
    AuthResult Verify(const AuthRequest& req) override {
        AuthResult r;
        if (!m_mod) {
            r.deny_reason = "auth_internal";
            return r;
        }
        const int rc = m_mod->CallAuthVerify(req.token);
        if (rc == 1) {
            r.allow = true;
            r.uid = req.token; // v1：token 即身份
            r.ttl_s = 3600;
        } else {
            r.deny_reason = (rc == 0) ? "wasm_deny" : "auth_internal";
        }
        return r;
    }

private:
    Wasm3Module* m_mod; // 生命周期由 WasmModuleManager 持有
};

} // namespace mediator::ext
