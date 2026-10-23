#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace client {

struct TerrainLodProduct final {
    std::shared_ptr<shared::TerrainWorld const> world;
    std::shared_ptr<shared::TerrainLodArtifact const> root;
};

class TerrainLodSource final {
public:
    using Builder = std::function<TerrainLodProduct(
        shared::TerrainSessionIdentity const&,
        std::stop_token
    )>;

    TerrainLodSource();
    explicit TerrainLodSource(Builder builder);
    ~TerrainLodSource();

    TerrainLodSource(TerrainLodSource const&) = delete;
    TerrainLodSource& operator=(TerrainLodSource const&) = delete;

    [[nodiscard]] bool request(shared::TerrainSessionIdentity const& session);
    void cancel();

    [[nodiscard]] std::shared_ptr<shared::TerrainWorld const> worldFor(
        shared::TerrainSessionIdentity const& session
    ) const noexcept;
    [[nodiscard]] std::shared_ptr<shared::TerrainLodArtifact const> rootFor(
        shared::TerrainSessionIdentity const& session
    ) const noexcept;

private:
    struct Request final {
        uint64_t generation = 0U;
        shared::TerrainSessionIdentity session;
    };

    struct Publication final {
        uint64_t generation = 0U;
        shared::TerrainSessionIdentity session;
        TerrainLodProduct product;
    };

    void workerLoop();
    [[nodiscard]] std::shared_ptr<Publication const> publicationFor(
        shared::TerrainSessionIdentity const& session
    ) const noexcept;

    Builder m_builder;
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_work_available;
    std::optional<Request> m_pending = std::nullopt;
    std::optional<Request> m_active_request = std::nullopt;
    std::shared_ptr<std::stop_source> m_active_cancellation = nullptr;
    std::atomic<uint64_t> m_latest_generation{ 0U };
    std::shared_ptr<Publication const> m_publication;
    uint64_t m_next_generation = 0U;
    bool m_stopping = false;
};

} // namespace client
