#pragma once

#include <shared/net/Message.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace shared {

// Desktop single-player shares immutable illustration snapshots between its
// two threads. The ordered preparation messages still travel over the wire.
class LocalPreparationSnapshots final {
public:
    void bind(TerrainSessionIdentity const& session)
    {
        std::lock_guard lock{ m_mutex };
        if (m_session != session) {
            m_session = session;
            m_pending.clear();
        }
    }

    [[nodiscard]] bool retain(
        TerrainSessionIdentity const& session,
        uint64_t const sequence,
        std::shared_ptr<generation::GenerationSnapshot const> const& snapshot
    )
    {
        std::lock_guard lock{ m_mutex };
        return m_session == session && sequence != 0U && snapshot
            && m_pending.try_emplace(sequence, snapshot).second;
    }

    [[nodiscard]] std::shared_ptr<generation::GenerationSnapshot const> take(
        TerrainSessionIdentity const& session,
        uint64_t const sequence
    )
    {
        std::lock_guard lock{ m_mutex };
        if (m_session != session) {
            return nullptr;
        }
        auto pending = m_pending.extract(sequence);
        return pending.empty() ? nullptr : std::move(pending.mapped());
    }

    void clear()
    {
        std::lock_guard lock{ m_mutex };
        m_pending.clear();
        m_session.reset();
    }

private:
    std::mutex m_mutex;
    std::optional<TerrainSessionIdentity> m_session;
    std::map<uint64_t, std::shared_ptr<generation::GenerationSnapshot const>> m_pending;
};

} // namespace shared
