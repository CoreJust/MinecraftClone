#include <core/IO/File.hpp>
#include <core/IO/JoinContainerFmt.hpp>
#include <core/IO/OptionalFmt.hpp>
#include <core/IO/TaggedBoolFmt.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

TEST(FileTest, ReadsExistingFile) {
    std::filesystem::path const path = std::filesystem::temp_directory_path() / "minecraft_clone_io_test.txt";
    {
        std::ofstream file{ path, std::ios::binary };
        file << "hello";
    }

    auto const content = core::readFile(path.string());

    ASSERT_TRUE(content.has_value());
    std::string const text{ content->begin(), content->end() };
    EXPECT_EQ(text, "hello");
    std::filesystem::remove(path);
}

TEST(FileTest, MissingFileReturnsNullopt) {
    auto const content = core::readFile(
        (std::filesystem::temp_directory_path() / "minecraft_clone_missing_file").string()
    );
    EXPECT_FALSE(content.has_value());
}

TEST(FileTest, ReadsIntoExistingBuffer) {
    std::filesystem::path const path = std::filesystem::temp_directory_path() / "minecraft_clone_io_buffer.txt";
    {
        std::ofstream file{ path, std::ios::binary };
        file << "data";
    }

    std::array<uint8_t, 4> buffer{ };
    EXPECT_TRUE(core::readFileTo(path.string(), buffer));
    std::string const text{ buffer.begin(), buffer.end() };
    EXPECT_EQ(text, "data");
    std::filesystem::remove(path);
}
