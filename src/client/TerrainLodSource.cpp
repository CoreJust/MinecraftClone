#include "TerrainLodSource.hpp"

#include <client/S2MapPresentation.hpp>
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
#include <thread>
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
TerrainLodProduct buildTerrainLodRoot(
    shared::TerrainSessionIdentity const& session,
    std::stop_token const& cancellation,
    PreparedTerrainWorldProvider const& prepared_world_provider
)
{
    std::shared_ptr<shared::TerrainWorld const> world;
    if (prepared_world_provider) {
        using namespace std::chrono_literals;
        while (!cancellation.stop_requested()) {
            PreparedTerrainWorldStatus const status = prepared_world_provider(session);
            if (cancellation.stop_requested()) {
                throw generation::GenerationCancelled{};
            }
            switch (status.state) {
                case PreparedTerrainWorldState::Pending:
                    std::this_thread::sleep_for(25ms);
                    continue;
                case PreparedTerrainWorldState::Ready:
                    world = status.world;
                    break;
                case PreparedTerrainWorldState::Failed:
                    throw std::runtime_error{ status.error.empty()
                        ? "Local terrain world preparation failed."
                        : status.error };
                case PreparedTerrainWorldState::Cancelled:
                    throw generation::GenerationCancelled{};
            }
            break;
        }
        if (cancellation.stop_requested()) {
            throw generation::GenerationCancelled{};
        }
        if (!world
            || world->identity() != session.terrain
            || world->atlas()->configuration != session.configuration) {
            throw std::runtime_error{ "Prepared terrain world does not match the active server session." };
        }
    } else {
        std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
            session.configuration,
            cancellation
        );
        if (!atlas || atlas->configuration != session.configuration) {
            throw std::runtime_error{ "Generated atlas does not match the requested terrain configuration." };
        }

        world = std::make_shared<shared::TerrainWorld const>(atlas, session.terrain.revision);
        if (world->identity() != session.terrain) {
            throw std::runtime_error{ "Generated terrain identity does not match the active server session." };
        }
    }
    auto const root = shared::buildTerrainLod(
        *world,
        shared::TerrainLodLevel::L0,
        0U,
        0U,
        generation::WORLD_SIZE_BLOCKS,
        cancellation
    );
    if (cancellation.stop_requested()) {
        throw generation::GenerationCancelled{};
    }
    if (!root) {
        throw std::runtime_error{ "Terrain LOD source could not build the full L0 root." };
    }
    return {
        .world = world,
        .root = root,
    };
}

[[nodiscard]]
std::shared_ptr<S2MapImage const> buildDefaultMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& root,
    std::stop_token const cancellation
)
{
    auto map = rasterizeS2WorldMap(world, root, S2WorldMapState{}, cancellation);
    return map ? std::make_shared<S2MapImage const>(std::move(*map)) : nullptr;
}

[[nodiscard]]
TerrainLodSource::Builder makePreparedWorldBuilder(PreparedTerrainWorldProvider provider)
{
    if (!provider) {
        throw std::invalid_argument{ "Prepared terrain LOD source requires a world provider." };
    }
    return [provider = std::move(provider)](
        shared::TerrainSessionIdentity const& session,
        std::stop_token const& cancellation
    ) {
        return buildTerrainLodRoot(session, cancellation, provider);
    };
}

[[nodiscard]]
bool isValidMap(S2MapImage const& map, shared::TerrainLodArtifact const& root)
{
    return map.source_identity == root.terrain_identity
        && map.source_model_identity == root.model_identity
        && map.width != 0U
        && map.height != 0U
        && map.rgba.size() == static_cast<uint64_t>(map.width) * map.height;
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
        || product.root->model_identity != shared::terrainLodModel(session.configuration.model_tag)
        || product.root->level != shared::TerrainLodLevel::L0
        || product.root->origin_x != 0U
        || product.root->origin_y != 0U
        || product.root->extent != generation::WORLD_SIZE_BLOCKS) {
        return false;
    }
    if (product.map && !isValidMap(*product.map, *product.root)) {
        return false;
    }

    uint32_t const spacing = shared::terrainLodSpacing(
        session.configuration.resolution,
        shared::TerrainLodLevel::L0,
        session.configuration.model_tag
    );
    uint32_t const side = generation::WORLD_SIZE_BLOCKS / spacing + 1U;
    return product.root->spacing == spacing
        && product.root->side == side
        && product.root->nodes.size() == static_cast<uint64_t>(side) * side;
}

} // namespace

TerrainLodSource::TerrainLodSource()
    : TerrainLodSource{
        [](shared::TerrainSessionIdentity const& session, std::stop_token const& cancellation) {
            return buildTerrainLodRoot(session, cancellation, {});
        },
        buildDefaultMap,
    }
{
}

TerrainLodSource::TerrainLodSource(Builder builder)
    : TerrainLodSource{ std::move(builder), {} }
{
}

TerrainLodSource::TerrainLodSource(Builder builder, MapBuilder map_builder)
    : m_builder{ std::move(builder) }
    , m_map_builder{ std::move(map_builder) }
{
    if (!m_builder) {
        throw std::invalid_argument{ "Terrain LOD source requires a builder." };
    }
    m_worker = std::thread{ [this] { workerLoop(); } };
}

TerrainLodSource::TerrainLodSource(PreparedTerrainWorldProvider provider)
    : TerrainLodSource{ std::move(provider), buildDefaultMap }
{
}

TerrainLodSource::TerrainLodSource(PreparedTerrainWorldProvider provider, MapBuilder map_builder)
    : TerrainLodSource{ makePreparedWorldBuilder(std::move(provider)), std::move(map_builder) }
{
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

std::shared_ptr<S2MapImage const> TerrainLodSource::mapFor(
    shared::TerrainSessionIdentity const& session
) const noexcept
{
    std::shared_ptr<Publication const> const publication = publicationFor(session);
    return publication ? publication->product.map : nullptr;
}

bool TerrainLodSource::mapUnavailableFor(shared::TerrainSessionIdentity const& session) const noexcept
{
    std::lock_guard lock{ m_mutex };
    uint64_t const generation = m_latest_generation.load(std::memory_order_acquire);
    return m_publication && m_publication->generation == generation && m_publication->session == session
        && !m_publication->product.map && (!m_active_request || m_active_request->generation != generation);
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
        bool root_published = false;
        try {
            std::stop_token const token = cancellation->get_token();
            TerrainLodProduct product = m_builder(request.session, token);
            if (token.stop_requested()) {
                throw generation::GenerationCancelled{};
            }
            if (!isValidProduct(product, request.session)) {
                throw std::runtime_error{ "Terrain LOD builder returned an incomplete or mismatched root." };
            }

            publish(request, product, cancellation);
            root_published = true;
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at
            );
            CORE_INFO(
                "Client L0 terrain source ready: seed={}, resolution={}, elapsed_ms={}",
                request.session.terrain.seed,
                request.session.configuration.resolution,
                elapsed.count()
            );

            if (m_map_builder && !token.stop_requested()) {
                auto const map_started_at = std::chrono::steady_clock::now();
                product.map = m_map_builder(*product.world, *product.root, token);
                if (token.stop_requested()) {
                    throw generation::GenerationCancelled{};
                }
                if (!product.map || !isValidMap(*product.map, *product.root)) {
                    throw std::runtime_error{ "Terrain LOD source could not prepare a matching default S2 map." };
                }
                publish(request, product, cancellation);
                auto const map_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - map_started_at
                );
                CORE_INFO(
                    "Client default S2 map ready: seed={}, resolution={}, elapsed_ms={}",
                    request.session.terrain.seed,
                    request.session.configuration.resolution,
                    map_elapsed.count()
                );
            }
        } catch (generation::GenerationCancelled const&) {
        } catch (std::exception const& error) {
            if (!cancellation->stop_requested()
                && request.generation == m_latest_generation.load(std::memory_order_acquire)) {
                CORE_ERROR(
                    "Client {} failed for seed {}: {}",
                    root_published ? "default S2 map" : "L0 terrain source",
                    request.session.terrain.seed,
                    error.what()
                );
            }
        } catch (...) {
            if (!cancellation->stop_requested()
                && request.generation == m_latest_generation.load(std::memory_order_acquire)) {
                CORE_ERROR(
                    "Client {} failed for seed {} with an unknown error.",
                    root_published ? "default S2 map" : "L0 terrain source",
                    request.session.terrain.seed
                );
            }
        }

        {
            std::lock_guard lock{ m_mutex };
            if (m_active_cancellation == cancellation) {
                m_active_cancellation.reset();
                m_active_request.reset();
            }
        }
    }
}

void TerrainLodSource::publish(
    Request const& request,
    TerrainLodProduct const& product,
    std::shared_ptr<std::stop_source> const& cancellation
)
{
    auto publication = std::make_shared<Publication const>(Publication{
        .generation = request.generation,
        .session = request.session,
        .product = product,
    });
    std::lock_guard lock{ m_mutex };
    if (m_stopping
        || cancellation->stop_requested()
        || m_active_cancellation != cancellation
        || request.generation != m_latest_generation.load(std::memory_order_acquire)) {
        throw generation::GenerationCancelled{};
    }
    m_publication = std::move(publication);
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
