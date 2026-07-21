// ============================================================================
// wasm3_host.h — wasm3 沙箱宿主（设计文档 §7.2/§7.3，运行时选型 wasm3）
//
// 实现思路：
//   wasm3 是单文件解释器（无 JIT），体积小、启动快，适合网关注入式扩展。
//   每个扩展模块拥有独立 Runtime（内存/栈隔离），加载失败不影响宿主。
//
// 认证 ABI（WasmAuth 使用，§7.3.2）：
//   wasm 模块需导出:
//     memory                        —— 线性内存（宿主在固定偏移写入 token）
//     auth_verify(i32 ptr, i32 len) -> i32   —— 1=允许 0=拒绝
//   宿主流程：m3_GetMemory 取得线性内存 → 将 token 写到偏移 kTokenOffset
//   → 调用 auth_verify(kTokenOffset, len) → 按返回值构造 AuthResult。
//   fail-closed：加载失败/函数缺失/trap 一律拒绝，deny_reason="auth_internal"。
//   v1 简化：允许时 uid = token 原文（由模块语义保证 token 即身份）。
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

    const std::string& LastError() const { return m_lastError; }

    static constexpr uint32_t kTokenOffset = 1024; // token 写入线性内存的固定偏移
    static constexpr size_t kRuntimeMemSize = 64 * 1024;

private:
    M3Environment* m_env = nullptr;
    M3Runtime* m_rt = nullptr;
    M3Function* m_authVerify = nullptr;
    std::string m_lastError;
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
