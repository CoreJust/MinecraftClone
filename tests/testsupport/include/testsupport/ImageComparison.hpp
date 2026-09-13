#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace testsupport {

inline constexpr uint64_t MAX_IMAGE_BYTE_COUNT = 64U * 1'024U * 1'024U;

struct Rgba8Image final {
    uint32_t width = 0U;
    uint32_t height = 0U;
    bool srgb_encoded = false;
    std::vector<uint8_t> pixels;
};

struct ImageComparisonPolicy final {
    uint8_t channel_tolerance = 0U;
    uint64_t max_mismatched_pixels = 0U;
};

struct ImageComparisonResult final {
    bool matches = false;
    bool dimensions_match = false;
    uint64_t mismatched_pixels = 0U;
    uint8_t maximum_channel_delta = 0U;
    uint32_t first_mismatch_x = 0U;
    uint32_t first_mismatch_y = 0U;
    std::string diagnostic;
};

[[nodiscard]]
std::expected<Rgba8Image, std::string> readPpm(std::filesystem::path const& file_path);

[[nodiscard]]
std::expected<std::monostate, std::string> writePpm(
    Rgba8Image const& image,
    std::filesystem::path const& file_path,
    std::string const& provenance
);

[[nodiscard]]
ImageComparisonResult compareRgba8(
    Rgba8Image const& expected,
    Rgba8Image const& actual,
    ImageComparisonPolicy policy
);

[[nodiscard]]
std::expected<std::monostate, std::string> writeMismatchArtifacts(
    Rgba8Image const& expected,
    Rgba8Image const& actual,
    ImageComparisonResult const& result,
    ImageComparisonPolicy policy,
    std::filesystem::path const& output_directory,
    std::string const& artifact_stem
);

} // namespace testsupport
