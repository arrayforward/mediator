// ============================================================================
// protocol_plugin.cpp — wasm 协议插件宿主实现（协议无关原语）
//
// 宿主原语用 wasm3 m3ApiRawFunction 风格编写。wasm3 不给 raw 函数传
// userdata，用 thread_local 保存"当前插件实例"（连接线程内串行，无重入）。
// ============================================================================
#include "ext/protocol_plugin.h"

#include <m3_api_defs.h>
#include <wasm3.h>

#include <chrono>
#include <nlohmann/json.hpp>

#include "audio/align.h"
#include "audio/g711.h"
#include "core/log.h"

namespace mediator::ext {

namespace {
thread_local ProtocolPlugin* g_self = nullptr;

std::string MemStr(uint8_t* mem, uint32_t off, uint32_t len) {
    return {reinterpret_cast<char*>(mem + off), len};
}
} // namespace

// ---- env 原语（wasm3 raw API）----

m3ApiRawFunction(RawSendText) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, len);
    m3ApiReturn(g_self->HostSendText(p, len));
}

m3ApiRawFunction(RawSendBinary) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, len);
    m3ApiReturn(g_self->HostSendBinary(p, len));
}

m3ApiRawFunction(RawInject) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, type);
    m3ApiGetArg(uint32_t, flags);
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, len);
    uint8_t* mem = static_cast<uint8_t*>(_mem);
    m3ApiReturn(g_self->HostInject(type, flags, MemStr(mem, p, len)));
}

m3ApiRawFunction(RawJsonGet) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, jp);
    m3ApiGetArg(uint32_t, jl);
    m3ApiGetArg(uint32_t, kp);
    m3ApiGetArg(uint32_t, kl);
    m3ApiGetArg(uint32_t, out);
    uint8_t* mem = static_cast<uint8_t*>(_mem);
    m3ApiReturn(g_self->HostJsonGet(MemStr(mem, jp, jl), MemStr(mem, kp, kl), out));
}

m3ApiRawFunction(RawResampleG711) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, ip);
    m3ApiGetArg(uint32_t, il);
    m3ApiGetArg(uint32_t, from);
    m3ApiGetArg(uint32_t, to);
    m3ApiGetArg(uint32_t, out);
    m3ApiReturn(g_self->HostResampleG711(ip, il, from, to, out));
}

m3ApiRawFunction(RawBindSession) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, len);
    uint8_t* mem = static_cast<uint8_t*>(_mem);
    m3ApiReturn(g_self->HostBindSession(MemStr(mem, p, len)));
}

m3ApiRawFunction(RawClose) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, code);
    m3ApiReturn(g_self->HostClose(static_cast<uint16_t>(code)));
}

m3ApiRawFunction(RawUplinkAsr) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, flags);
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, len);
    m3ApiReturn(g_self->HostUplinkAsr(flags, p, len));
}

m3ApiRawFunction(RawFeedWatermark) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, len);
    m3ApiReturn(g_self->HostFeedWatermark(p, len) ? 1 : 0);
}

m3ApiRawFunction(RawNowMs) {
    m3ApiReturnType(int64_t);
    m3ApiReturn(g_self->HostNowMs());
}

// ---- ProtocolPlugin ----

ProtocolPlugin::ProtocolPlugin() = default;
ProtocolPlugin::~ProtocolPlugin() = default;

void ProtocolPlugin::SetActiveThis() { g_self = this; }

bool ProtocolPlugin::Load(const std::string& path, Hooks hooks) {
    m_hooks = std::move(hooks);
    m_mod = std::make_unique<Wasm3Module>();
    if (!m_mod->LoadFromFile(path)) {
        m_lastError = m_mod->LastError();
        MDT_ERROR("protocol plugin load failed: {}", m_lastError);
        m_mod.reset();
        return false;
    }
    struct { const char* name; const char* sig; const void* fn; } links[] = {
        {"host_send_text", "i(ii)", reinterpret_cast<const void*>(&RawSendText)},
        {"host_send_binary", "i(ii)", reinterpret_cast<const void*>(&RawSendBinary)},
        {"host_inject", "i(iiii)", reinterpret_cast<const void*>(&RawInject)},
        {"host_json_get", "i(iiiii)", reinterpret_cast<const void*>(&RawJsonGet)},
        {"host_resample_g711", "i(iiiii)", reinterpret_cast<const void*>(&RawResampleG711)},
        {"host_bind_session", "i(ii)", reinterpret_cast<const void*>(&RawBindSession)},
        {"host_close", "i(i)", reinterpret_cast<const void*>(&RawClose)},
        {"host_uplink_asr", "i(iii)", reinterpret_cast<const void*>(&RawUplinkAsr)},
        {"host_feed_watermark", "i(ii)", reinterpret_cast<const void*>(&RawFeedWatermark)},
        {"host_now_ms", "I()", reinterpret_cast<const void*>(&RawNowMs)},
    };
    for (const auto& l : links) {
        if (!m_mod->LinkHostFunction(l.name, l.sig, l.fn)) {
            MDT_WARN("protocol plugin link {}: {}", l.name, m_mod->LastError());
        }
    }
    if (!m_mod->HasFunction("on_ws_text") || !m_mod->HasFunction("on_ws_binary")) {
        m_lastError = "plugin missing on_ws_text/on_ws_binary exports: " +
                      m_mod->LastError();
        m_mod.reset();
        return false;
    }
    MDT_INFO("protocol plugin loaded: {}", path);
    return true;
}

bool ProtocolPlugin::SetConfigKey(const std::string& key) {
    if (!m_mod || key.size() > 64) return false;
    const uint32_t len = static_cast<uint32_t>(key.size());
    if (!m_mod->WriteMemory(kConfigKeyOffset, &len, sizeof(len))) return false;
    return m_mod->WriteMemory(kConfigKeyOffset + 4, key.data(), key.size());
}

bool ProtocolPlugin::WriteIn(const void* data, size_t len) {
    return m_mod->WriteMemory(kInOffset, data, len);
}

// ---- 帧事件派发 ----

void ProtocolPlugin::OnWsText(const std::string& data) {
    if (!m_mod || !WriteIn(data.data(), data.size())) return;
    SetActiveThis();
    m_mod->CallExportI32("on_ws_text", {std::to_string(kInOffset),
                                        std::to_string(data.size())});
}

void ProtocolPlugin::OnWsBinary(const uint8_t* data, size_t len) {
    if (!m_mod || !WriteIn(data, len)) return;
    SetActiveThis();
    m_mod->CallExportI32("on_ws_binary", {std::to_string(kInOffset),
                                          std::to_string(len)});
}

void ProtocolPlugin::OnOutboundClip(ClipId clip, const std::vector<uint8_t>& g711_16k) {
    if (!m_mod || !WriteIn(g711_16k.data(), g711_16k.size())) return;
    SetActiveThis();
    m_mod->CallExportI32("on_outbound_clip",
                         {std::to_string(clip), std::to_string(kInOffset),
                          std::to_string(g711_16k.size())});
}

void ProtocolPlugin::OnOutboundText(const std::string& text) {
    if (!m_mod || !WriteIn(text.data(), text.size())) return;
    SetActiveThis();
    m_mod->CallExportI32("on_outbound_text", {std::to_string(kInOffset),
                                              std::to_string(text.size())});
}

void ProtocolPlugin::OnLlmText(const std::string& text) {
    if (!m_mod || !WriteIn(text.data(), text.size())) return;
    SetActiveThis();
    m_mod->CallExportI32("on_llm_text", {std::to_string(kInOffset),
                                         std::to_string(text.size())});
}

void ProtocolPlugin::OnThinking() {
    if (!m_mod) return;
    SetActiveThis();
    m_mod->CallExportI32("on_thinking", {});
}

// ---- 宿主原语实现 ----

int32_t ProtocolPlugin::HostSendText(uint32_t ptr, uint32_t len) {
    uint32_t mem_size = 0;
    uint8_t* mem = m_mod->Memory(&mem_size);
    if (!mem || ptr + len > mem_size) return -1;
    if (m_hooks.send_text)
        m_hooks.send_text({reinterpret_cast<char*>(mem + ptr), len});
    return 0;
}

int32_t ProtocolPlugin::HostSendBinary(uint32_t ptr, uint32_t len) {
    uint32_t mem_size = 0;
    uint8_t* mem = m_mod->Memory(&mem_size);
    if (!mem || ptr + len > mem_size) return -1;
    if (m_hooks.send_binary)
        m_hooks.send_binary({mem + ptr, mem + ptr + len});
    return 0;
}

int32_t ProtocolPlugin::HostInject(uint32_t type, uint32_t flags,
                                   const std::string& text) {
    if (m_hooks.inject)
        m_hooks.inject(static_cast<MsgType>(type), flags, text);
    return 0;
}

int32_t ProtocolPlugin::HostJsonGet(const std::string& json, const std::string& key,
                                    uint32_t out) {
    try {
        const auto j = nlohmann::json::parse(json);
        // 顶层优先；信封类 JSON 回退到 body 内层（hello 等协议字段在 body 里）
        const nlohmann::json* f = nullptr;
        if (j.contains(key)) f = &j[key];
        else if (j.contains("body") && j["body"].is_object() && j["body"].contains(key))
            f = &j["body"][key];
        if (!f) return -1;
        const std::string v = f->is_string() ? f->get<std::string>() : f->dump();
        if (!m_mod->WriteMemory(out, v.data(), v.size())) return -1;
        return static_cast<int32_t>(v.size());
    } catch (...) {
        return -1;
    }
}

int32_t ProtocolPlugin::HostResampleG711(uint32_t ip, uint32_t il, uint32_t from,
                                         uint32_t to, uint32_t out) {
    uint32_t mem_size = 0;
    uint8_t* mem = m_mod->Memory(&mem_size);
    if (!mem || ip + il > mem_size) return -1;
    const std::vector<uint8_t> in(mem + ip, mem + ip + il);
    const auto converted = audio::ResampleG711(in, static_cast<int>(from),
                                               static_cast<int>(to));
    if (!m_mod->WriteMemory(out, converted.data(), converted.size())) return -1;
    return static_cast<int32_t>(converted.size());
}

int32_t ProtocolPlugin::HostBindSession(const std::string& uid) {
    if (uid.empty()) return -1;
    if (m_hooks.bind_session) m_hooks.bind_session(uid);
    return 0;
}

int32_t ProtocolPlugin::HostClose(uint16_t code) {
    if (m_hooks.close) m_hooks.close(code);
    return 0;
}

int32_t ProtocolPlugin::HostUplinkAsr(uint32_t flags, uint32_t ptr, uint32_t len) {
    uint32_t mem_size = 0;
    uint8_t* mem = m_mod->Memory(&mem_size);
    if (!mem || ptr + len > mem_size) return -1;
    // 协议侧 8k G.711A → 内部 16k PCM 转 ASR（含 VAD/端点 flags）
    const auto pcm = audio::G711ToPcmResampled(mem + ptr, len, 8000, 16000);
    if (m_hooks.forward_asr) m_hooks.forward_asr(pcm, flags);
    return 0;
}

bool ProtocolPlugin::HostFeedWatermark(uint32_t ptr, uint32_t len) {
    uint32_t mem_size = 0;
    uint8_t* mem = m_mod->Memory(&mem_size);
    if (!mem || ptr + len > mem_size) return false;
    return m_hooks.feed_watermark ? m_hooks.feed_watermark(mem + ptr, len) : false;
}

int64_t ProtocolPlugin::HostNowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace mediator::ext
