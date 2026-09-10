#include <core/vulkan/SpirV.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> bytesFromWords(std::span<uint32_t const> words) {
    std::vector<uint8_t> bytes(words.size_bytes());
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

} // namespace

TEST(SpirVTest, CopiesUnalignedByteInputIntoAlignedWords) {
    static constexpr std::array<uint32_t, 3> WORDS{ 0xDEAD'BEEFu, 42u, 0x1234'5678u };

    auto const bytes = bytesFromWords(WORDS);
    std::vector<uint8_t> unaligned_bytes(1 + bytes.size());
    std::memcpy(unaligned_bytes.data() + 1, bytes.data(), bytes.size());

    core::vk::SpirV const spirv{ std::span<uint8_t const>{ unaligned_bytes }.subspan(1) };
    std::span<uint32_t const> const data = spirv.data();

    ASSERT_EQ(data.size(), WORDS.size());
    EXPECT_EQ(reinterpret_cast<uintptr_t>(data.data()) % alignof(uint32_t), 0u);
    EXPECT_EQ(data[0], WORDS[0]);
    EXPECT_EQ(data[1], WORDS[1]);
    EXPECT_EQ(data[2], WORDS[2]);
    EXPECT_EQ(std::memcmp(data.data(), bytes.data(), bytes.size()), 0);
}

TEST(SpirVTest, OwnsWordsAfterCallerStorageChangesAndLifetime) {
    static constexpr std::array<uint32_t, 3> WORDS{ 0xDEAD'BEEFu, 42u, 0x1234'5678u };

    core::vk::SpirV const spirv = [] {
        std::vector<uint8_t> bytes = bytesFromWords(WORDS);
        core::vk::SpirV loaded{ bytes };
        std::fill(bytes.begin(), bytes.end(), 0u);
        return loaded;
    }();

    EXPECT_EQ(spirv.data()[0], WORDS[0]);
    EXPECT_EQ(spirv.data()[1], WORDS[1]);
    EXPECT_EQ(spirv.data()[2], WORDS[2]);
}

TEST(SpirVTest, EmptyPayloadIsAccepted) {
    std::vector<uint8_t> bytes;
    core::vk::SpirV spirv{ bytes };
    EXPECT_TRUE(spirv.data().empty());
    EXPECT_EQ(spirv.data().size_bytes(), 0u);
    EXPECT_EQ(spirv.data().size(), 0u);
}

TEST(SpirVTest, SingleWordRoundTrips) {
    static constexpr std::array<uint32_t, 1> WORDS{ 0xCAFE'BABEu };
    auto bytes = bytesFromWords(WORDS);
    core::vk::SpirV spirv{ bytes };
    ASSERT_EQ(spirv.data().size(), 1u);
    EXPECT_EQ(spirv.data()[0], 0xCAFE'BABEu);
}

TEST(SpirVTest, ManyWordsPreserveOrderAndValues) {
    static constexpr std::array<uint32_t, 8> WORDS{ 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    auto bytes = bytesFromWords(WORDS);
    core::vk::SpirV spirv{ bytes };
    ASSERT_EQ(spirv.data().size(), WORDS.size());
    for (uint32_t i = 0; i < WORDS.size(); ++i) {
        EXPECT_EQ(spirv.data()[i], WORDS[i]) << "word " << i;
    }
}

TEST(SpirVTest, LargePayloadSizeMatchesByteCount) {
    static constexpr uint32_t WORD_COUNT = 1024;
    std::vector<uint32_t> words(WORD_COUNT, 0xA5A5'A5A5u);
    auto bytes = bytesFromWords(words);
    uint64_t const byte_count = bytes.size();
    core::vk::SpirV spirv{ bytes };
    EXPECT_EQ(spirv.data().size(), WORD_COUNT);
    EXPECT_EQ(spirv.data().size_bytes(), byte_count);
    EXPECT_EQ(spirv.data()[0], 0xA5A5'A5A5u);
    EXPECT_EQ(spirv.data()[WORD_COUNT - 1], 0xA5A5'A5A5u);
}

TEST(SpirVTest, DataViewSizeIsBytesDividedByFour) {
    for (uint32_t words = 1; words <= 16; ++words) {
        std::vector<uint32_t> payload(words, 0u);
        auto bytes = bytesFromWords(payload);
        core::vk::SpirV spirv{ bytes };
        EXPECT_EQ(spirv.data().size(), words) << "words=" << words;
        EXPECT_EQ(spirv.data().size_bytes(), words * 4u) << "words=" << words;
    }
}

TEST(SpirVTest, RejectsPayloadsWithPartialWords) {
    std::vector<uint8_t> bytes(3);
    std::string const previous_death_test_style = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");

    EXPECT_EXIT(
        static_cast<void>(core::vk::SpirV{ bytes }),
        ::testing::ExitedWithCode(1),
        ""
    );

    GTEST_FLAG_SET(death_test_style, previous_death_test_style);
}
