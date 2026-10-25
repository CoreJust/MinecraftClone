#include "../../src/client/TerrainLodSource.hpp"

#include <shared/net/Message.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]]
shared::TerrainSessionIdentity makeSession(uint64_t const seed, uint64_t const revision, uint64_t const join_epoch)
{
    generation::GenerationConfiguration const configuration{
        .seed = seed,
        .resolution = 256U,
    };
    return shared::TerrainSessionIdentity{
        .join_epoch = join_epoch,
        .terrain = shared::TerrainIdentity{
            .terrain_version = generation::TERRAIN_VERSION,
            .revision = revision,
            .seed = seed,
            .configuration_identity = generation::configurationIdentity(configuration),
        },
        .configuration = configuration,
    };
}

[[nodiscard]]
std::shared_ptr<shared::TerrainWorld const> makeWorld(shared::TerrainSessionIdentity const& session)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration = session.configuration;
    atlas->vertical_scale = 10.0;
    uint32_t const count = session.configuration.resolution * session.configuration.resolution;
    atlas->elevation.assign(count, 1.5F);
    atlas->temperature.assign(count, 12.0F);
    atlas->precipitation.assign(count, 0.3F);
    atlas->water_level.assign(count, 0.0F);
    atlas->ocean_mask.assign(count, 0U);
    atlas->lake_mask.assign(count, 0U);
    atlas->river_mask.assign(count, 0U);

    return std::make_shared<shared::TerrainWorld const>(atlas, session.terrain.revision);
}

[[nodiscard]]
client::TerrainLodProduct makeProduct(
    shared::TerrainSessionIdentity const& session,
    std::stop_token const& cancellation = {}
)
{
    auto const world = makeWorld(session);
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
bool waitForPublication(
    client::TerrainLodSource const& source,
    shared::TerrainSessionIdentity const& session,
    std::chrono::milliseconds const timeout
)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (source.worldFor(session) && source.rootFor(session)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

TEST(TerrainLodSourceTest, RejectsConfigurationIdentityMismatchBeforeStartingWork)
{
    std::atomic<uint32_t> builder_calls{ 0U };
    client::TerrainLodSource source{ [&](shared::TerrainSessionIdentity const& session, std::stop_token const& cancellation) {
        ++builder_calls;
        return makeProduct(session, cancellation);
    } };
    shared::TerrainSessionIdentity session = makeSession(42U, 7U, 1U);
    session.terrain.configuration_identity.append("-stale");

    EXPECT_FALSE(source.request(session));
    EXPECT_FALSE(source.worldFor(session));
    EXPECT_FALSE(source.rootFor(session));
    EXPECT_EQ(builder_calls.load(), 0U);
}

TEST(TerrainLodSourceTest, PublishesCompleteImmutableRootForExactSessionAndCancelClearsIt)
{
    std::atomic<uint32_t> builder_calls{ 0U };
    client::TerrainLodSource source{ [&](shared::TerrainSessionIdentity const& session, std::stop_token const& cancellation) {
        ++builder_calls;
        return makeProduct(session, cancellation);
    } };
    shared::TerrainSessionIdentity const session = makeSession(42U, 7U, 1U);

    ASSERT_TRUE(source.request(session));
    ASSERT_TRUE(waitForPublication(source, session, 2s));
    EXPECT_TRUE(source.request(session));
    EXPECT_EQ(builder_calls.load(), 1U);

    std::shared_ptr<shared::TerrainWorld const> const world = source.worldFor(session);
    std::shared_ptr<shared::TerrainLodArtifact const> const root = source.rootFor(session);
    ASSERT_NE(world, nullptr);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(world->identity(), session.terrain);
    EXPECT_EQ(root->terrain_identity, session.terrain);
    EXPECT_EQ(root->level, shared::TerrainLodLevel::L0);
    EXPECT_EQ(root->origin_x, 0U);
    EXPECT_EQ(root->origin_y, 0U);
    EXPECT_EQ(root->extent, generation::WORLD_SIZE_BLOCKS);
    EXPECT_EQ(root->side, 257U);
    EXPECT_EQ(root->nodes.size(), 257U * 257U);

    source.cancel();
    EXPECT_FALSE(source.worldFor(session));
    EXPECT_FALSE(source.rootFor(session));
}

TEST(TerrainLodSourceTest, CancelledSessionCannotPublishAfterReplacementRequest)
{
    struct BuilderState final {
        std::mutex mutex;
        std::condition_variable_any changed;
        bool first_started = false;
        std::atomic<uint32_t> calls{ 0U };
    } state;
    client::TerrainLodSource source{ [&state](
        shared::TerrainSessionIdentity const& session,
        std::stop_token const& cancellation
    ) {
        ++state.calls;
        if (session.join_epoch == 1U) {
            std::unique_lock lock{ state.mutex };
            state.first_started = true;
            state.changed.notify_all();
            static_cast<void>(state.changed.wait(lock, cancellation, [] { return false; }));
            return makeProduct(session);
        }
        return makeProduct(session, cancellation);
    } };
    shared::TerrainSessionIdentity const first = makeSession(42U, 7U, 1U);
    shared::TerrainSessionIdentity const second = makeSession(42U, 7U, 2U);

    ASSERT_TRUE(source.request(first));
    {
        std::unique_lock lock{ state.mutex };
        ASSERT_TRUE(state.changed.wait_for(lock, 2s, [&state] { return state.first_started; }));
    }
    EXPECT_FALSE(source.worldFor(first));
    ASSERT_TRUE(source.request(second));
    EXPECT_FALSE(source.worldFor(first));
    EXPECT_FALSE(source.rootFor(first));
    EXPECT_FALSE(source.worldFor(second));

    ASSERT_TRUE(waitForPublication(source, second, 2s));
    EXPECT_FALSE(source.worldFor(first));
    EXPECT_FALSE(source.rootFor(first));
    ASSERT_NE(source.worldFor(second), nullptr);
    ASSERT_NE(source.rootFor(second), nullptr);
    EXPECT_EQ(source.worldFor(second)->identity(), second.terrain);
    EXPECT_EQ(state.calls.load(), 2U);
}

TEST(TerrainLodSourceTest, DefaultBuilderPreparesMatchingAtlasAndFullWorldL0)
{
    client::TerrainLodSource source;
    shared::TerrainSessionIdentity const session = makeSession(42U, 7U, 1U);

    ASSERT_TRUE(source.request(session));
    ASSERT_TRUE(waitForPublication(source, session, 60s));
    std::shared_ptr<shared::TerrainWorld const> const world = source.worldFor(session);
    std::shared_ptr<shared::TerrainLodArtifact const> const root = source.rootFor(session);
    ASSERT_NE(world, nullptr);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(world->atlas()->configuration, session.configuration);
    EXPECT_EQ(world->identity(), session.terrain);
    EXPECT_EQ(root->terrain_identity, session.terrain);
    EXPECT_EQ(root->model_identity, shared::TERRAIN_LOD_MODEL);
    EXPECT_EQ(root->level, shared::TerrainLodLevel::L0);
    EXPECT_EQ(root->extent, generation::WORLD_SIZE_BLOCKS);
    EXPECT_EQ(root->side, 257U);
    EXPECT_EQ(root->nodes.size(), 257U * 257U);
}

TEST(TerrainLodSourceTest, PreparedWorldProviderBuildsL0FromTheSinglePreparedWorld)
{
    shared::TerrainSessionIdentity const session = makeSession(42U, 7U, 1U);
    uint32_t prepared_world_factory_calls = 0U;
    auto const prepare_world = [&] {
        ++prepared_world_factory_calls;
        return makeWorld(session);
    };
    std::shared_ptr<shared::TerrainWorld const> const prepared_world = prepare_world();
    std::atomic<uint32_t> provider_calls{ 0U };
    client::TerrainLodSource source{ [&](shared::TerrainSessionIdentity const& requested_session) {
        ++provider_calls;
        if (requested_session != session) {
            return client::PreparedTerrainWorldStatus{
                .state = client::PreparedTerrainWorldState::Failed,
                .error = "unexpected local client session",
            };
        }
        return client::PreparedTerrainWorldStatus{
            .state = client::PreparedTerrainWorldState::Ready,
            .world = prepared_world,
        };
    } };

    ASSERT_TRUE(source.request(session));
    ASSERT_TRUE(waitForPublication(source, session, 2s));

    std::shared_ptr<shared::TerrainWorld const> const client_world = source.worldFor(session);
    ASSERT_NE(client_world, nullptr);
    EXPECT_EQ(prepared_world_factory_calls, 1U);
    EXPECT_EQ(provider_calls.load(), 1U);
    EXPECT_EQ(client_world, prepared_world);
    EXPECT_EQ(client_world->identity(), session.terrain);
    EXPECT_EQ(client_world->sampleSurfaceHeight(4'096U, 8'192U),
        prepared_world->sampleSurfaceHeight(4'096U, 8'192U));
    ASSERT_NE(source.rootFor(session), nullptr);
    EXPECT_EQ(source.rootFor(session)->terrain_identity, prepared_world->identity());
}

TEST(TerrainLodSourceTest, PreparedWorldProviderStopsOnTerminalStateAndRejectsMismatchedWorld)
{
    shared::TerrainSessionIdentity const session = makeSession(42U, 7U, 1U);
    for (client::PreparedTerrainWorldState const terminal_state : {
            client::PreparedTerrainWorldState::Failed,
            client::PreparedTerrainWorldState::Cancelled,
        }) {
        std::atomic<uint32_t> provider_calls{ 0U };
        client::TerrainLodSource source{ [&](shared::TerrainSessionIdentity const&) {
            ++provider_calls;
            return client::PreparedTerrainWorldStatus{
                .state = terminal_state,
                .error = "preparation ended",
            };
        } };
        ASSERT_TRUE(source.request(session));
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (provider_calls.load() == 0U && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(provider_calls.load(), 1U);
        std::this_thread::sleep_for(30ms);
        EXPECT_EQ(provider_calls.load(), 1U);
        EXPECT_FALSE(source.worldFor(session));
        EXPECT_FALSE(source.rootFor(session));
    }

    shared::TerrainSessionIdentity const wrong_session = makeSession(43U, 7U, 1U);
    std::shared_ptr<shared::TerrainWorld const> const mismatched_world = makeWorld(wrong_session);
    std::atomic<uint32_t> mismatched_provider_calls{ 0U };
    client::TerrainLodSource mismatched_source{ [&](shared::TerrainSessionIdentity const&) {
        ++mismatched_provider_calls;
        return client::PreparedTerrainWorldStatus{
            .state = client::PreparedTerrainWorldState::Ready,
            .world = mismatched_world,
        };
    } };
    ASSERT_TRUE(mismatched_source.request(session));
    auto const mismatch_deadline = std::chrono::steady_clock::now() + 2s;
    while (mismatched_provider_calls.load() == 0U && std::chrono::steady_clock::now() < mismatch_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(mismatched_provider_calls.load(), 1U);
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(mismatched_source.worldFor(session));
    EXPECT_FALSE(mismatched_source.rootFor(session));
}

TEST(TerrainLodSourceTest, ClientCancellationStopsWaitingWithoutCancellingOrOutlivingPreparation)
{
    struct PreparationLifetime final {
        std::atomic_bool cancelled{ false };
        std::atomic<uint32_t> provider_calls{ 0U };
    };
    auto preparation = std::make_shared<PreparationLifetime>();
    std::weak_ptr<PreparationLifetime> const weak_preparation = preparation;
    shared::TerrainSessionIdentity const session = makeSession(42U, 7U, 1U);
    {
        client::TerrainLodSource source{ [preparation](shared::TerrainSessionIdentity const&) {
            ++preparation->provider_calls;
            return client::PreparedTerrainWorldStatus{};
        } };
        preparation.reset();
        ASSERT_TRUE(source.request(session));
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            std::shared_ptr<PreparationLifetime> const current_preparation = weak_preparation.lock();
            if (!current_preparation || current_preparation->provider_calls.load() != 0U) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        std::shared_ptr<PreparationLifetime> const current_preparation = weak_preparation.lock();
        ASSERT_NE(current_preparation, nullptr);
        source.cancel();
        EXPECT_FALSE(current_preparation->cancelled.load());
    }

    EXPECT_TRUE(weak_preparation.expired());
}
