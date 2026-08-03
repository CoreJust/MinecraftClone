#include <core/vulkan/SpirV.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

std::vector<uint8_t> bytesFromWords(std::span<uint32_t const> words) {
    std::vector<uint8_t> bytes(words.size_bytes());
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

} // namespace

TEST(SpirVTest, ConstructorMovesPayloadOutOfSource) {
    uint32_t const words[] = { 0xDEAD'BEEFu, 42u, 0x1234'5678u };
    auto bytes = bytesFromWords(words);
    size_t const original_size = bytes.size();

    core::vk::SpirV spirv{ std::move(bytes) };

    EXPECT_TRUE(bytes.empty());
    auto const data = spirv.data();
    EXPECT_EQ(data.size_bytes(), original_size);
    ASSERT_EQ(data.size(), std::size(words));
    EXPECT_EQ(data[0], 0xDEAD'BEEFu);
    EXPECT_EQ(data[1], 42u);
    EXPECT_EQ(data[2], 0x1234'5678u);
}

TEST(SpirVTest, EmptyPayloadIsAccepted) {
    std::vector<uint8_t> bytes;
    core::vk::SpirV spirv{ std::move(bytes) };
    EXPECT_TRUE(spirv.data().empty());
    EXPECT_EQ(spirv.data().size_bytes(), 0u);
    EXPECT_EQ(spirv.data().size(), 0u);
}

TEST(SpirVTest, SingleWordRoundTrips) {
    uint32_t const word = 0xCAFE'BABEu;
    auto bytes = bytesFromWords(std::span{ &word, 1 });
    core::vk::SpirV spirv{ std::move(bytes) };
    ASSERT_EQ(spirv.data().size(), 1u);
    EXPECT_EQ(spirv.data()[0], 0xCAFE'BABEu);
}

TEST(SpirVTest, ManyWordsPreserveOrderAndValues) {
    uint32_t const words[] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    auto bytes = bytesFromWords(words);
    core::vk::SpirV spirv{ std::move(bytes) };
    ASSERT_EQ(spirv.data().size(), std::size(words));
    for (size_t i = 0; i < std::size(words); ++i) {
        EXPECT_EQ(spirv.data()[i], words[i]) << "word " << i;
    }
}

TEST(SpirVTest, LargePayloadSizeMatchesByteCount) {
    constexpr size_t word_count = 1024;
    std::vector<uint32_t> words(word_count, 0xA5A5'A5A5u);
    auto bytes = bytesFromWords(words);
    size_t const byte_count = bytes.size();
    core::vk::SpirV spirv{ std::move(bytes) };
    EXPECT_EQ(spirv.data().size(), word_count);
    EXPECT_EQ(spirv.data().size_bytes(), byte_count);
    EXPECT_EQ(spirv.data()[0], 0xA5A5'A5A5u);
    EXPECT_EQ(spirv.data()[word_count - 1], 0xA5A5'A5A5u);
}

TEST(SpirVTest, DataViewSizeIsBytesDividedByFour) {
    for (size_t words = 1; words <= 16; ++words) {
        std::vector<uint32_t> payload(words, 0u);
        auto bytes = bytesFromWords(payload);
        core::vk::SpirV spirv{ std::move(bytes) };
        EXPECT_EQ(spirv.data().size(), words) << "words=" << words;
        EXPECT_EQ(spirv.data().size_bytes(), words * 4u) << "words=" << words;
    }
}
