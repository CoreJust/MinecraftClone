#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>

#include <core/net/Client.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

class PreviewClient final : public core::Client {
public:
    PreviewClient()
        : core::Client{2}
    { }

    std::vector<shared::Message> messages;

private:
    void onDisconnected(core::DisconnectEvent const) override { }

    void onReceived(core::ReceiveEvent event) override
    {
        auto const message = shared::decodeMessage(event.data);
        if (message) {
            messages.push_back(*message);
        }
    }
};

uint32_t previewCount(std::vector<shared::Message> const& messages)
{
    return static_cast<uint32_t>(std::ranges::count_if(messages, [](shared::Message const& message) {
        return std::holds_alternative<shared::ServerChunkPreviewMessage>(message);
    }));
}

} // namespace

TEST(GameServerPreviewTest, SendsCoarseCoverageThenAtMostAdvertisedFinalBudget)
{
    static constexpr std::chrono::seconds TIMEOUT{15};
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr uint32_t MAX_PREVIEW_CHUNKS = 320U;
    static constexpr uint32_t COARSE_CHUNKS = 25U;

    server::GameServer server{0, {}, shared::WorldMode::Flight};
    std::atomic_bool stop_requested{false};
    std::thread server_thread{[&server, &stop_requested] {
        server.run(stop_requested);
    }};
    PreviewClient client;
    ASSERT_TRUE(client.connect(core::Address::localhost(server.port()), TIMEOUT));
    ASSERT_TRUE(client.send(shared::encodeMessage(shared::JoinRequestMessage{
        .ch = '@',
        .mode = shared::WorldMode::Flight,
        .wants_previews = true,
    }), 0, core::SendMode{core::SendMode::Reliable}));

    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (previewCount(client.messages) < MAX_PREVIEW_CHUNKS
        && std::chrono::steady_clock::now() < deadline) {
        client.poll(POLL_INTERVAL);
    }
    stop_requested.store(true, std::memory_order_relaxed);
    server_thread.join();

    ASSERT_EQ(previewCount(client.messages), MAX_PREVIEW_CHUNKS);
    uint32_t coarse_count = 0U;
    uint32_t final_count = 0U;
    bool sent_final = false;
    uint32_t descriptor_count = 0U;
    for (shared::Message const& message : client.messages) {
        if (auto const* const descriptor = std::get_if<shared::ServerPreviewDescriptorMessage>(&message)) {
            ++descriptor_count;
            EXPECT_EQ(descriptor->max_preview_chunks, MAX_PREVIEW_CHUNKS);
            continue;
        }
        auto const* const preview = std::get_if<shared::ServerChunkPreviewMessage>(&message);
        if (preview == nullptr) {
            continue;
        }
        EXPECT_EQ(preview->bytes.size(), shared::Chunk::BLOCK_COUNT);
        EXPECT_EQ(preview->length, shared::Chunk::BLOCK_COUNT);
        if (preview->level == shared::PreviewLevel::Coarse) {
            EXPECT_FALSE(sent_final);
            ++coarse_count;
        } else {
            sent_final = true;
            ++final_count;
        }
    }
    EXPECT_EQ(descriptor_count, 1U);
    EXPECT_EQ(coarse_count, COARSE_CHUNKS);
    EXPECT_EQ(final_count, MAX_PREVIEW_CHUNKS - COARSE_CHUNKS);
}
