// ============================================================================
// test_wasm3.cpp — wasm3 宿主与 wasm 认证扩展单测（设计文档 §7.3.3）
//
// 用例：加载 allow 模块（正确 token 放行/错误拒绝）、trap 模块 fail-closed、
//       模块热更新（替换后新逻辑生效）、缺失模块拒绝加载且保留旧实例。
// .wasm 由 wat2wasm 在构建期生成（见 tests/CMakeLists.txt）；无工具时跳过。
// ============================================================================
#include <gtest/gtest.h>

#include <fstream>

#include "core/clock.h"
#include "ext/wasm3_host.h"

using mediator::ext::AuthRequest;
using mediator::ext::Wasm3Module;
using mediator::ext::WasmAuth;
using mediator::ext::WasmModuleManager;

namespace {

bool FileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

const char* kAllowWasm = MEDIATOR_WASM_DIR "/auth_allow.wasm";
const char* kTrapWasm = MEDIATOR_WASM_DIR "/auth_trap.wasm";

} // namespace

TEST(Wasm3Auth, AllowModuleAcceptsMagicToken) {
    if (!FileExists(kAllowWasm)) GTEST_SKIP() << "wat2wasm 不可用";
    Wasm3Module mod;
    ASSERT_TRUE(mod.LoadFromFile(kAllowWasm)) << mod.LastError();
    WasmAuth auth(&mod);
    const auto ok = auth.Verify(AuthRequest{"wasm-ok", "", 0});
    EXPECT_TRUE(ok.allow);
    EXPECT_EQ(ok.uid, "wasm-ok"); // v1：token 即身份
}

TEST(Wasm3Auth, AllowModuleRejectsWrongToken) {
    if (!FileExists(kAllowWasm)) GTEST_SKIP();
    Wasm3Module mod;
    ASSERT_TRUE(mod.LoadFromFile(kAllowWasm));
    WasmAuth auth(&mod);
    const auto r = auth.Verify(AuthRequest{"bad-token", "", 0});
    EXPECT_FALSE(r.allow);
    EXPECT_EQ(r.deny_reason, "wasm_deny");
}

TEST(Wasm3Auth, TrapModuleFailsClosed) {
    if (!FileExists(kTrapWasm)) GTEST_SKIP();
    Wasm3Module mod;
    ASSERT_TRUE(mod.LoadFromFile(kTrapWasm));
    WasmAuth auth(&mod);
    const auto r = auth.Verify(AuthRequest{"wasm-ok", "", 0});
    EXPECT_FALSE(r.allow);
    EXPECT_EQ(r.deny_reason, "auth_internal"); // trap → 内部错误 → fail-closed
}

TEST(Wasm3Auth, NullModuleFailsClosed) {
    WasmAuth auth(nullptr);
    EXPECT_FALSE(auth.Verify(AuthRequest{"wasm-ok", "", 0}).allow);
}

TEST(Wasm3Manager, LoadReplaceAndMissingKeepsOld) {
    if (!FileExists(kAllowWasm) || !FileExists(kTrapWasm)) GTEST_SKIP();
    WasmModuleManager mgr;
    ASSERT_TRUE(mgr.Load("auth", kAllowWasm));
    EXPECT_EQ(mgr.Count(), 1u);
    EXPECT_EQ(mgr.Find("auth")->CallAuthVerify("wasm-ok"), 1);
    // 热更新为 trap 模块：替换后新逻辑生效
    ASSERT_TRUE(mgr.Load("auth", kTrapWasm));
    EXPECT_EQ(mgr.Count(), 1u);
    EXPECT_LT(mgr.Find("auth")->CallAuthVerify("wasm-ok"), 0);
    // 加载不存在路径：失败且保留旧实例
    EXPECT_FALSE(mgr.Load("auth", "/nonexistent/x.wasm"));
    EXPECT_NE(mgr.Find("auth"), nullptr);
}
