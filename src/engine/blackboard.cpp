#include "engine/blackboard.h"

namespace mediator {

SessionContext& DataBoard::GetOrCreateSession(const SessionId& sid) {
    auto it = m_sessions.find(sid);
    if (it == m_sessions.end()) {
        SessionContext ctx;
        ctx.m_sessionId = sid;
        it = m_sessions.emplace(sid, std::move(ctx)).first;
    }
    return it->second;
}

SessionContext* DataBoard::FindSession(const SessionId& sid) {
    auto it = m_sessions.find(sid);
    return it == m_sessions.end() ? nullptr : &it->second;
}

void DataBoard::RemoveSession(const SessionId& sid) { m_sessions.erase(sid); }

bool DataBoard::RegisterOnline(const std::string& uid, const std::string& gw, uint64_t gen) {
    auto& e = m_online[uid];
    if (gen < e.generation) return false; // 旧代际拒绝
    e.gw_id = gw;
    e.generation = gen;
    return true;
}

void DataBoard::SetOffline(const std::string& uid) { m_online.erase(uid); }

std::optional<OnlineEntry> DataBoard::Lookup(const std::string& uid) const {
    auto it = m_online.find(uid);
    if (it == m_online.end()) return std::nullopt;
    return it->second;
}

std::unordered_set<SessionId, SessionIdHash> DataBoard::TakeChangedForEvolve() {
    auto s = std::move(m_changed);
    m_changed.clear();
    return s;
}

void DataBoard::Inject(const SessionId& sid, const SessionContext& ctx) {
    m_sessions[sid] = ctx;
}

} // namespace mediator
