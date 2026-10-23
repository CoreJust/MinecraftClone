#include "TerrainLodSource.hpp"

#include <shared/net/Message.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>
#include <core/IO/Log.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client {

namespace {

[[nodiscard]]
bool isValidSession(shared::TerrainSessionIdentity const& session) noexcept
{
    if (session.join_epoch == 0U
        || session.terrain.revision == 0U
        || !shared::isValidTerrainIdentity(session.terrain)
        || session.terrain.seed != session.configuration.seed
        || (session.configuration.resolution != 256U && session.configuration.resolution != 512U)) {
        return false;
    }
    try {
        return session.terrain.configuration_identity
            == generation::configurationIdentity(session.configuration);
    } catch (...) {
        return false;
    }
}

[[nodiscard]]
TerrainLodProduct buildTerrainLodProduct(
    shared::TerrainSessionIdentity const& session,
    std::stop_token const& cancellation
)
{
    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
        session.configuration,
        cancellation
    );
    if (!atlas || atlas->configuration != session.configuration) {
        throw std::runtime_error{ "Generated atlas does not match the requested terrain configuration." };
    }

    auto const world = std::make_shared<shared::TerrainWorld const>(atlas, session.terrain.revision);
    if (world->identity() != session.terrain) {
        throw std::runtime_error{ "Generated terrain identity does not match the active server session." };
    }
    auto const root = shared::buildTerrainLod(
        *world,
        shared::TerrainLodLevel::L0,
        0U,
        0U,
        generation::WORLD_SIZE_BLOCKS,
        cancellation
    );
    return { .world = world, .root = root };
}

[[nodiscard]]
bool isValidProduct(
    TerrainLodProduct const& product,
    shared::TerrainSessionIdentity const& session
)
{
    if (!product.world || !product.root
        || product.world->identity() != session.terrain
        || product.world->atlas()->configuration != session.configuration
        || product.root->terrain_identity != session.terrain
        || product.root->model_identity != shared::TERRAIN_LOD_MODEL
        || product.root->level != shared::TerrainLodLevel::L0
        || product.root->origin_x != 0U
        || product.root->origin_y != 0U
        || product.root->extent != generation::WORLD_SIZE_BLOCKS) {
        return false;
    }

    uint32_t const spacing = shared::terrainLodSpacing(
        session.configuration.resolution,
        shared::TerrainLodLevel::L0
    );
    uint32_t const side = generation::WORLD_SIZE_BLOCKS / spacing + 1U;
    return product.root->spacing == spacing
        && product.root->side == side
        && product.root->nodes.size() == static_cast<uint64_t>(side) * side;
}

} // namespace

TerrainLodSource::TerrainLodSource()
    : TerrainLodSource{ buildTerrainLodProduct }
{
}

TerrainLodSource::TerrainLodSource(Builder builder)
    : m_builder{ std::move(builder) }
{
    if (!m_builder) {
        throw std::invalid_argument{ "Terrain LOD source requires a builder." };
    }
    m_worker = std::thread{ [this] { workerLoop(); } };
}

TerrainLodSource::~TerrainLodSource()
{
    {
        std::lock_guard lock{ m_mutex };
        m_stopping = true;
        m_pending.reset();
        ++m_next_generation;
        m_latest_generation.store(m_next_generation, std::memory_order_release);
        if (m_active_cancellation) {
            m_active_cancellation->request_stop();
        }
        m_publication.reset();
    }
    m_work_available.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool TerrainLodSource::request(shared::TerrainSessionIdentity const& session)
{
    if (!isValidSession(session)) {
        return false;
    }

    {
        std::lock_guard lock{ m_mutex };
        if (m_stopping) {
            return false;
        }
        if (m_pending && m_pending->session == session) {
            return true;
        }
        if (m_active_request && m_active_request->session == session
            && m_active_cancellation && !m_active_cancellation->stop_requested()
            && m_active_request->generation == m_latest_generation.load(std::memory_order_acquire)) {
            return true;
        }
        std::shared_ptr<Publication const> const publication = m_publication;
        if (publication
            && publication->generation == m_latest_generation.load(std::memory_order_acquire)
            && publication->session == session) {
            return true;
        }

        ++m_next_generation;
        if (m_next_generation == 0U) {
            ++m_next_generation;
        }
        m_latest_generation.store(m_next_generation, std::memory_order_release);
        if (m_active_cancellation) {
            m_active_cancellation->request_stop();
        }
        m_publication.reset();
        m_pending = Request{ .generation = m_next_generation, .session = session };
    }
    m_work_available.notify_one();
    return true;
}

void TerrainLodSource::cancel()
{
    {
        std::lock_guard lock{ m_mutex };
        if (m_stopping) {
            return;
        }
        ++m_next_generation;
        if (m_next_generation == 0U) {
            ++m_next_generation;
        }
        m_latest_generation.store(m_next_generation, std::memory_order_release);
        m_pending.reset();
        if (m_active_cancellation) {
            m_active_cancellation->request_stop();
        }
        m_publication.reset();
    }
    m_work_available.notify_one();
}

std::shared_ptr<shared::TerrainWorld const> TerrainLodSource::worldFor(
    shared::TerrainSessionIdentity const& session
) const noexcept
{
    std::shared_ptr<Publication const> const publication = publicationFor(session);
    return publication ? publication->product.world : nullptr;
}

std::shared_ptr<shared::TerrainLodArtifact const> TerrainLodSource::rootFor(
    shared::TerrainSessionIdentity const& session
) const noexcept
{
    std::shared_ptr<Publication const> const publication = publicationFor(session);
    return publication ? publication->product.root : nullptr;
}

void TerrainLodSource::workerLoop()
{
    while (true) {
        Request request;
        std::shared_ptr<std::stop_source> cancellation;
        {
            std::unique_lock lock{ m_mutex };
            m_work_available.wait(lock, [this] { return m_stopping || m_pending.has_value(); });
            if (m_stopping) {
                return;
            }
            if (!m_pending.has_value()) {
                continue;
            }
            request = std::move(m_pending.value());
            m_pending.reset();
            m_active_request = request;
            cancellation = std::make_shared<std::stop_source>();
            m_active_cancellation = cancellation;
        }

        auto const started_at = std::chrono::steady_clock::now();
        try {
            TerrainLodProduct product = m_builder(request.session, cancellation->get_token());
            if (cancellation->stop_requested()) {
                std::lock_guard lock{ m_mutex };
                if (m_active_cancellation == cancellation) {
                    m_active_cancellation.reset();
                    m_active_request.reset();
                }
                continue;
            }
            if (!isValidProduct(product, request.session)) {
                throw std::runtime_error{ "Terrain LOD builder returned an incomplete or mismatched root." };
            }

            auto const publication = std::make_shared<Publication const>(Publication{
                .generation = request.generation,
                .session = request.session,
                .product = std::move(product),
            });
            bool published = false;
            {
                std::lock_guard lock{ m_mutex };
                if (!m_stopping
                    && !cancellation->stop_requested()
                    && m_active_cancellation == cancellation
                    && request.generation == m_latest_generation.load(std::memory_order_acquire)) {
                    m_publication = publication;
                    published = true;
                }
                if (m_active_cancellation == cancellation) {
                    m_active_cancellation.reset();
                    m_active_request.reset();
                }
            }
            if (published) {
                auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at
                );
                CORE_INFO(
                    "Client L0 terrain source ready: seed={}, resolution={}, elapsed_ms={}",
                    request.session.terrain.seed,
                    request.session.configuration.resolution,
                    elapsed.count()
                );
            }
        } catch (generation::GenerationCancelled const&) {
            std::lock_guard lock{ m_mutex };
            if (m_active_cancellation == cancellation) {
                m_active_cancellation.reset();
                m_active_request.reset();
            }
        } catch (std::exception const& error) {
            {
                std::lock_guard lock{ m_mutex };
                if (m_active_cancellation == cancellation) {
                    m_active_cancellation.reset();
                    m_active_request.reset();
                }
            }
            if (!cancellation->stop_requested()
                && request.generation == m_latest_generation.load(std::memory_order_acquire)) {
                CORE_ERROR(
                    "Client L0 terrain source failed for seed {}: {}",
                    request.session.terrain.seed,
                    error.what()
                );
            }
        } catch (...) {
            std::lock_guard lock{ m_mutex };
            if (m_active_cancellation == cancellation) {
                m_active_cancellation.reset();
                m_active_request.reset();
            }
        }
    }
}

std::shared_ptr<TerrainLodSource::Publication const> TerrainLodSource::publicationFor(
    shared::TerrainSessionIdentity const& session
) const noexcept
{
    std::lock_guard lock{ m_mutex };
    uint64_t const generation = m_latest_generation.load(std::memory_order_acquire);
    std::shared_ptr<Publication const> publication = m_publication;
    if (generation != m_latest_generation.load(std::memory_order_acquire)
        || !publication
        || publication->generation != generation
        || publication->session != session) {
        return nullptr;
    }
    return publication;
}

} // namespace client
