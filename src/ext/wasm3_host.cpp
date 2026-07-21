// ============================================================================
// wasm3_host.cpp — wasm3 沙箱宿主实现
//
// 关键逻辑：
//   1. Load: m3_NewEnvironment → m3_ParseModule → m3_NewRuntime(内存上限)
//      → m3_LoadModule → m3_FindFunction("auth_verify")（认证模块必须导出）
//   2. CallAuthVerify: m3_GetMemory 取得线性内存，token 写入固定偏移
//      kTokenOffset，m3_CallArgv 调用，m3_GetResults 取 i32 返回值。
//      任何一步失败返回 -1（调用方 fail-closed 视为拒绝）。
//   3. 卸载：m3_FreeRuntime + m3_FreeEnvironment；模块字节流不驻留。
// ============================================================================
#include "ext/wasm3_host.h"

#include <cstring>
#include <fstream>

#include <wasm3.h>

namespace mediator::ext {

Wasm3Module::Wasm3Module() = default;

Wasm3Module::~Wasm3Module() {
    if (m_rt) m3_FreeRuntime(m_rt);
    if (m_env) m3_FreeEnvironment(m_env);
}

bool Wasm3Module::Load(const std::vector<uint8_t>& bytes) {
    if (m_rt) { // 重复加载：先释放旧实例（热更新由 Manager 建新实例后替换）
        m3_FreeRuntime(m_rt);
        m3_FreeEnvironment(m_env);
        m_rt = nullptr;
        m_env = nullptr;
        m_authVerify = nullptr;
    }
    m_env = m3_NewEnvironment();
    if (!m_env) {
        m_lastError = "m3_NewEnvironment failed";
        return false;
    }
    IM3Module mod = nullptr;
    M3Result pr = m3_ParseModule(m_env, &mod, bytes.data(),
                                 static_cast<uint32_t>(bytes.size()));
    if (pr) {
        m_lastError = std::string("parse: ") + pr;
        return false;
    }
    m_rt = m3_NewRuntime(m_env, kRuntimeMemSize, nullptr);
    if (!m_rt) {
        m_lastError = "m3_NewRuntime failed";
        return false;
    }
    M3Result lr = m3_LoadModule(m_rt, mod);
    if (lr) {
        m_lastError = std::string("load: ") + lr;
        return false;
    }
    // 认证模块必须导出 auth_verify；查找失败仅记录（观察者类模块可无此导出）
    m3_FindFunction(&m_authVerify, m_rt, "auth_verify");
    return true;
}

bool Wasm3Module::LoadFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        m_lastError = "open failed: " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    return Load(bytes);
}

int Wasm3Module::CallAuthVerify(const std::string& token) {
    if (!m_rt || !m_authVerify) return -1;
    uint32_t mem_size = 0;
    uint8_t* mem = m3_GetMemory(m_rt, &mem_size, 0);
    if (!mem || kTokenOffset + token.size() + 1 > mem_size) return -1;
    std::memcpy(mem + kTokenOffset, token.data(), token.size());
    mem[kTokenOffset + token.size()] = 0;

    const std::string ptr_arg = std::to_string(kTokenOffset);
    const std::string len_arg = std::to_string(token.size());
    const char* argv[] = {ptr_arg.c_str(), len_arg.c_str()};
    M3Result cr = m3_CallArgv(m_authVerify, 2, argv);
    if (cr) {
        m_lastError = std::string("call: ") + cr; // trap 等 → fail-closed
        return -1;
    }
    uint32_t ret = 0;
    const void* rets[] = {&ret};
    M3Result gr = m3_GetResults(m_authVerify, 1, rets);
    if (gr) return -1;
    return static_cast<int>(ret);
}

// ---- WasmModuleManager ----

bool WasmModuleManager::Load(const std::string& name, const std::string& path) {
    auto mod = std::make_unique<Wasm3Module>();
    if (!mod->LoadFromFile(path)) return false; // 失败保留旧实例
    m_modules[name] = std::move(mod);
    return true;
}

void WasmModuleManager::Unload(const std::string& name) { m_modules.erase(name); }

Wasm3Module* WasmModuleManager::Find(const std::string& name) {
    auto it = m_modules.find(name);
    return it == m_modules.end() ? nullptr : it->second.get();
}

size_t WasmModuleManager::Count() const { return m_modules.size(); }

} // namespace mediator::ext
