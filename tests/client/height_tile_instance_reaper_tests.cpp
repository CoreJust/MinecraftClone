#include <client/render/HeightTileInstanceReaper.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct DestructionState final {
    std::thread::id owner = std::this_thread::get_id();
    std::atomic<uint32_t> destroyed{ 0U };
    std::atomic<bool> destroyed_on_owner{ false };
    std::promise<void> entered;
    std::shared_future<void> release;
};

struct Instance final {
    explicit Instance(std::shared_ptr<DestructionState> value) : state(std::move(value)) {}
    Instance(Instance&&) noexcept = default;
    Instance& operator=(Instance&&) noexcept = default;
    Instance(Instance const&) = delete;
    ~Instance() noexcept
    {
        if (state) {
            if (state->release.valid()) {
                state->entered.set_value();
                static_cast<void>(state->release.wait_for(std::chrono::seconds{ 2 }));
            }
            state->destroyed_on_owner.store(std::this_thread::get_id() == state->owner);
            state->destroyed.fetch_add(1U);
        }
    }
    std::shared_ptr<DestructionState> state;
};

TEST(HeightTileInstanceReaperTest, AcceptedStorageIsDestroyedOffOwnerAndDrainReleasesBytes)
{
    auto state = std::make_shared<DestructionState>();
    client::HeightTileInstanceReaper<Instance> reaper;
    std::vector<Instance> instances;
    instances.emplace_back(state);
    ASSERT_TRUE(reaper.retire(instances));
    EXPECT_EQ(instances.capacity(), 0U);
    reaper.drain();
    EXPECT_EQ(state->destroyed.load(), 1U);
    EXPECT_FALSE(state->destroyed_on_owner.load());
    EXPECT_EQ(reaper.outstandingBytes(), 0U);
    EXPECT_GE(reaper.highWaterBytes(), sizeof(Instance));
    EXPECT_EQ(reaper.synchronousFallbackCount(), 0U);
}

TEST(HeightTileInstanceReaperTest, ByteBackpressureRetainsRejectedNonemptyCallerStorage)
{
    static constexpr uint64_t MAXIMUM_BYTES = sizeof(Instance);
    auto blocked = std::make_shared<DestructionState>();
    auto rejected = std::make_shared<DestructionState>();
    std::promise<void> release;
    blocked->release = release.get_future().share();
    auto entered = blocked->entered.get_future();
    client::HeightTileInstanceReaper<Instance> reaper(MAXIMUM_BYTES);
    std::vector<Instance> first;
    first.emplace_back(blocked);
    ASSERT_TRUE(reaper.retire(first));
    EXPECT_EQ(entered.wait_for(std::chrono::seconds{ 1 }), std::future_status::ready);
    std::vector<Instance> second;
    second.emplace_back(rejected);
    EXPECT_FALSE(reaper.retire(second));
    EXPECT_EQ(second.size(), 1U);
    EXPECT_EQ(rejected->destroyed.load(), 0U);
    EXPECT_EQ(reaper.outstandingBytes(), MAXIMUM_BYTES);
    EXPECT_EQ(reaper.synchronousFallbackCount(), 1U);
    release.set_value();
    reaper.drain();
    EXPECT_TRUE(reaper.retire(second));
    reaper.drain();
    EXPECT_EQ(rejected->destroyed.load(), 1U);
    EXPECT_FALSE(rejected->destroyed_on_owner.load());
}

TEST(HeightTileInstanceReaperTest, OversizeFallbackLeavesOwnershipForSynchronousDestruction)
{
    auto state = std::make_shared<DestructionState>();
    client::HeightTileInstanceReaper<Instance> reaper(0U);
    {
        std::vector<Instance> instances;
        instances.emplace_back(state);
        EXPECT_FALSE(reaper.retire(instances));
        EXPECT_EQ(instances.size(), 1U);
        EXPECT_EQ(state->destroyed.load(), 0U);
    }
    EXPECT_EQ(state->destroyed.load(), 1U);
    EXPECT_TRUE(state->destroyed_on_owner.load());
    EXPECT_EQ(reaper.highWaterBytes(), 0U);
}

TEST(HeightTileInstanceReaperTest, JobBoundRejectsWithoutTakingCallerOwnership)
{
    auto blocked = std::make_shared<DestructionState>();
    std::promise<void> release;
    blocked->release = release.get_future().share();
    auto entered = blocked->entered.get_future();
    client::HeightTileInstanceReaper<Instance> reaper;
    std::vector<Instance> first;
    first.emplace_back(blocked);
    ASSERT_TRUE(reaper.retire(first));
    EXPECT_EQ(entered.wait_for(std::chrono::seconds{ 1 }), std::future_status::ready);
    for (uint32_t index = 1U; index < reaper.MAXIMUM_JOBS; ++index) {
        std::vector<Instance> queued;
        queued.emplace_back(std::make_shared<DestructionState>());
        EXPECT_TRUE(reaper.retire(queued));
    }
    auto rejected = std::make_shared<DestructionState>();
    std::vector<Instance> instances;
    instances.emplace_back(rejected);
    EXPECT_FALSE(reaper.retire(instances));
    EXPECT_EQ(instances.size(), 1U);
    EXPECT_EQ(rejected->destroyed.load(), 0U);
    EXPECT_EQ(reaper.synchronousFallbackCount(), 1U);
    release.set_value();
    reaper.drain();
    EXPECT_EQ(reaper.outstandingBytes(), 0U);
}

TEST(HeightTileInstanceReaperTest, OwnedStorageIsChargedUntilWholeExtractedNodeDestructionCompletes)
{
    using Entries = std::unordered_map<uint32_t, Instance>;
    using Node = Entries::node_type;
    static constexpr uint64_t OWNED_STORAGE_BYTES = sizeof(Entries::value_type) + 128U;
    auto state = std::make_shared<DestructionState>();
    std::promise<void> release;
    state->release = release.get_future().share();
    auto entered = state->entered.get_future();
    client::HeightTileInstanceReaper<Node> reaper;
    Entries entries;
    entries.emplace(1U, state);
    std::vector<Node> retired;
    retired.push_back(entries.extract(1U));
    ASSERT_TRUE(entries.empty());
    uint64_t const charged_bytes = static_cast<uint64_t>(retired.capacity()) * sizeof(Node) + OWNED_STORAGE_BYTES;

    ASSERT_TRUE(reaper.retire(retired, OWNED_STORAGE_BYTES));

    EXPECT_TRUE(retired.empty());
    EXPECT_EQ(entered.wait_for(std::chrono::seconds{ 1 }), std::future_status::ready);
    EXPECT_EQ(reaper.outstandingBytes(), charged_bytes);
    EXPECT_GE(reaper.highWaterBytes(), charged_bytes);
    release.set_value();
    reaper.drain();
    EXPECT_EQ(reaper.outstandingBytes(), 0U);
    EXPECT_EQ(state->destroyed.load(), 1U);
    EXPECT_FALSE(state->destroyed_on_owner.load());
}

TEST(HeightTileInstanceReaperTest, NestedStorageBackpressurePreservesCallerOwnership)
{
    auto state = std::make_shared<DestructionState>();
    std::vector<Instance> instances;
    instances.emplace_back(state);
    uint64_t const container_bytes = static_cast<uint64_t>(instances.capacity()) * sizeof(Instance);
    client::HeightTileInstanceReaper<Instance> reaper(container_bytes + 128U);

    EXPECT_FALSE(reaper.retire(instances, 129U));

    ASSERT_EQ(instances.size(), 1U);
    EXPECT_EQ(state->destroyed.load(), 0U);
    EXPECT_EQ(reaper.outstandingBytes(), 0U);
    EXPECT_EQ(reaper.highWaterBytes(), 0U);
    EXPECT_EQ(reaper.synchronousFallbackCount(), 1U);
}

TEST(HeightTileInstanceReaperTest, OverflowingNestedStorageChargeCannotTransferCallerOwnership)
{
    auto state = std::make_shared<DestructionState>();
    std::vector<Instance> instances;
    instances.emplace_back(state);
    client::HeightTileInstanceReaper<Instance> reaper;

    EXPECT_FALSE(reaper.retire(instances, std::numeric_limits<uint64_t>::max()));

    EXPECT_EQ(instances.size(), 1U);
    EXPECT_EQ(state->destroyed.load(), 0U);
    EXPECT_EQ(reaper.outstandingBytes(), 0U);
    EXPECT_EQ(reaper.synchronousFallbackCount(), 1U);
}

TEST(HeightTileInstanceReaperTest, DestructionDrainsAcceptedStorageWithoutExplicitDrain)
{
    auto state = std::make_shared<DestructionState>();
    {
        client::HeightTileInstanceReaper<Instance> reaper;
        std::vector<Instance> instances;
        instances.emplace_back(state);
        ASSERT_TRUE(reaper.retire(instances));
    }
    EXPECT_EQ(state->destroyed.load(), 1U);
    EXPECT_FALSE(state->destroyed_on_owner.load());
}

} // namespace
