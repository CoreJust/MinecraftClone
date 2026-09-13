#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace testsupport {
namespace {

enum class PpmColorSpace {
    Linear,
    Srgb,
};

struct PpmHeader final {
    std::optional<PpmColorSpace> color_space;
};

[[nodiscard]]
std::expected<uint64_t, std::string> checkedByteCount(
    uint32_t const width,
    uint32_t const height,
    uint32_t const channels
)
{
    if (width == 0U || height == 0U || channels == 0U) {
        return std::unexpected("image dimensions and channel count must be non-zero");
    }
    uint64_t const maximum = std::numeric_limits<uint64_t>::max();
    if (static_cast<uint64_t>(width) > maximum / static_cast<uint64_t>(height)) {
        return std::unexpected("image dimensions overflow byte count");
    }
    uint64_t const pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > maximum / static_cast<uint64_t>(channels)) {
        return std::unexpected("image channel count overflows byte count");
    }
    uint64_t const byte_count = pixels * static_cast<uint64_t>(channels);
    if (byte_count > MAX_IMAGE_BYTE_COUNT) {
        return std::unexpected("image exceeds the maximum byte count");
    }
    return byte_count;
}

[[nodiscard]]
std::expected<std::monostate, std::string> parseComment(
    std::string const& comment,
    PpmHeader& header
)
{
    static constexpr std::string_view COLOR_SPACE_PREFIX = " color-space=";
    if (!comment.starts_with(COLOR_SPACE_PREFIX)) {
        return std::monostate{};
    }
    if (header.color_space.has_value()) {
        return std::unexpected("duplicate PPM color-space declaration");
    }
    std::string_view const value = std::string_view{ comment }.substr(COLOR_SPACE_PREFIX.size());
    if (value == "linear") {
        header.color_space = PpmColorSpace::Linear;
        return std::monostate{};
    }
    if (value == "srgb") {
        header.color_space = PpmColorSpace::Srgb;
        return std::monostate{};
    }
    return std::unexpected("unknown PPM color-space declaration: " + std::string{ value });
}

[[nodiscard]]
std::expected<std::string, std::string> readToken(std::istream& input, PpmHeader& header)
{
    char character = '\0';
    while (input.get(character)) {
        if (character == '#') {
            std::string comment;
            std::getline(input, comment);
            if (std::expected<std::monostate, std::string> const parsed = parseComment(comment, header);
                !parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(character))) {
            break;
        }
    }
    if (!input) {
        return std::unexpected("unexpected end of PPM header");
    }
    std::string token;
    token += character;
    while (input.get(character) && !std::isspace(static_cast<unsigned char>(character))) {
        token += character;
    }
    return token;
}

[[nodiscard]]
std::expected<uint32_t, std::string> parseDimension(std::string const& token, char const* const label)
{
    try {
        uint64_t const parsed = std::stoull(token);
        if (parsed == 0U || parsed > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(std::string{ "invalid PPM " } + label);
        }
        return static_cast<uint32_t>(parsed);
    } catch (std::exception const&) {
        return std::unexpected(std::string{ "invalid PPM " } + label);
    }
}

[[nodiscard]]
std::expected<std::monostate, std::string> validateImage(Rgba8Image const& image)
{
    std::expected<uint64_t, std::string> const expected_size = checkedByteCount(
        image.width,
        image.height,
        4U
    );
    if (!expected_size.has_value()) {
        return std::unexpected(expected_size.error());
    }
    if (*expected_size != image.pixels.size()) {
        return std::unexpected("RGBA8 image dimensions do not match pixel bytes");
    }
    return std::monostate{};
}

[[nodiscard]]
uint8_t channelDelta(uint8_t const expected, uint8_t const actual) noexcept
{
    return expected > actual ? expected - actual : actual - expected;
}

[[nodiscard]]
Rgba8Image makeDiffImage(Rgba8Image const& expected, Rgba8Image const& actual, uint8_t const tolerance)
{
    Rgba8Image diff{
        .width = actual.width,
        .height = actual.height,
        .srgb_encoded = actual.srgb_encoded,
        .pixels = actual.pixels,
    };
    for (uint64_t offset = 0U; offset < diff.pixels.size(); offset += 4U) {
        uint8_t const red_delta = channelDelta(expected.pixels[offset], actual.pixels[offset]);
        uint8_t const green_delta = channelDelta(expected.pixels[offset + 1U], actual.pixels[offset + 1U]);
        uint8_t const blue_delta = channelDelta(expected.pixels[offset + 2U], actual.pixels[offset + 2U]);
        uint8_t const alpha_delta = channelDelta(expected.pixels[offset + 3U], actual.pixels[offset + 3U]);
        if (std::max({ red_delta, green_delta, blue_delta, alpha_delta }) <= tolerance) {
            diff.pixels[offset] = 0U;
            diff.pixels[offset + 1U] = 0U;
            diff.pixels[offset + 2U] = 0U;
        } else {
            diff.pixels[offset] = 255U;
            diff.pixels[offset + 1U] = red_delta;
            diff.pixels[offset + 2U] = blue_delta;
        }
        diff.pixels[offset + 3U] = 255U;
    }
    return diff;
}

} // namespace

std::expected<Rgba8Image, std::string> readPpm(std::filesystem::path const& file_path)
{
    std::ifstream input{ file_path, std::ios::binary };
    if (!input) {
        return std::unexpected("cannot open PPM reference: " + file_path.string());
    }
    PpmHeader header;
    std::expected<std::string, std::string> const magic = readToken(input, header);
    if (!magic.has_value() || *magic != "P6") {
        return std::unexpected(magic.has_value() ? "PPM reference is not P6" : magic.error());
    }
    std::expected<std::string, std::string> const width_token = readToken(input, header);
    std::expected<std::string, std::string> const height_token = readToken(input, header);
    std::expected<std::string, std::string> const max_value_token = readToken(input, header);
    if (!width_token.has_value()) {
        return std::unexpected(width_token.error());
    }
    if (!height_token.has_value()) {
        return std::unexpected(height_token.error());
    }
    if (!max_value_token.has_value()) {
        return std::unexpected(max_value_token.error());
    }
    std::expected<uint32_t, std::string> const width = parseDimension(*width_token, "width");
    std::expected<uint32_t, std::string> const height = parseDimension(*height_token, "height");
    if (!width.has_value() || !height.has_value() || *max_value_token != "255") {
        return std::unexpected("unsupported PPM dimensions or channel range");
    }
    if (!header.color_space.has_value()) {
        return std::unexpected("missing PPM color-space declaration");
    }
    std::expected<uint64_t, std::string> const rgb_byte_count = checkedByteCount(*width, *height, 3U);
    if (!rgb_byte_count.has_value()) {
        return std::unexpected(rgb_byte_count.error());
    }
    std::expected<uint64_t, std::string> const rgba_byte_count = checkedByteCount(*width, *height, 4U);
    if (!rgba_byte_count.has_value()) {
        return std::unexpected(rgba_byte_count.error());
    }
    std::vector<uint8_t> rgb(static_cast<size_t>(*rgb_byte_count));
    input.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (static_cast<uint64_t>(input.gcount()) != *rgb_byte_count) {
        return std::unexpected("PPM reference has incomplete pixel data");
    }
    Rgba8Image image{
        .width = *width,
        .height = *height,
        .srgb_encoded = *header.color_space == PpmColorSpace::Srgb,
        .pixels = {},
    };
    image.pixels.reserve(*rgba_byte_count);
    for (uint64_t offset = 0U; offset < rgb.size(); offset += 3U) {
        image.pixels.push_back(rgb[offset]);
        image.pixels.push_back(rgb[offset + 1U]);
        image.pixels.push_back(rgb[offset + 2U]);
        image.pixels.push_back(255U);
    }
    return image;
}

std::expected<std::monostate, std::string> writePpm(
    Rgba8Image const& image,
    std::filesystem::path const& file_path,
    std::string const& provenance
)
{
    if (std::expected<std::monostate, std::string> const valid = validateImage(image); !valid.has_value()) {
        return std::unexpected(valid.error());
    }
    std::error_code error;
    std::filesystem::create_directories(file_path.parent_path(), error);
    if (error) {
        return std::unexpected("cannot create PPM directory: " + error.message());
    }
    std::ofstream output{ file_path, std::ios::binary | std::ios::trunc };
    if (!output) {
        return std::unexpected("cannot write PPM output: " + file_path.string());
    }
    output << "P6\n# " << provenance << "\n# color-space="
        << (image.srgb_encoded ? "srgb" : "linear") << "\n"
        << image.width << ' ' << image.height << "\n255\n";
    for (uint64_t offset = 0U; offset < image.pixels.size(); offset += 4U) {
        std::array<char, 3> const rgb{
            static_cast<char>(image.pixels[offset]),
            static_cast<char>(image.pixels[offset + 1U]),
            static_cast<char>(image.pixels[offset + 2U]),
        };
        output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
    if (!output) {
        return std::unexpected("failed while writing PPM output: " + file_path.string());
    }
    return std::monostate{};
}

ImageComparisonResult compareRgba8(
    Rgba8Image const& expected,
    Rgba8Image const& actual,
    ImageComparisonPolicy const policy
)
{
    ImageComparisonResult result;
    if (std::expected<std::monostate, std::string> const expected_valid = validateImage(expected);
        !expected_valid.has_value()) {
        result.diagnostic = "invalid expected image: " + expected_valid.error();
        return result;
    }
    if (std::expected<std::monostate, std::string> const actual_valid = validateImage(actual);
        !actual_valid.has_value()) {
        result.diagnostic = "invalid actual image: " + actual_valid.error();
        return result;
    }
    result.dimensions_match = expected.width == actual.width && expected.height == actual.height;
    if (!result.dimensions_match) {
        result.diagnostic = "dimension mismatch: expected " + std::to_string(expected.width) + "x"
            + std::to_string(expected.height) + ", actual " + std::to_string(actual.width) + "x"
            + std::to_string(actual.height);
        return result;
    }
    if (expected.srgb_encoded != actual.srgb_encoded) {
        result.diagnostic = "color encoding mismatch: expected "
            + std::string{ expected.srgb_encoded ? "sRGB" : "linear" } + ", actual "
            + std::string{ actual.srgb_encoded ? "sRGB" : "linear" };
        return result;
    }
    for (uint64_t offset = 0U; offset < expected.pixels.size(); offset += 4U) {
        uint8_t const red_delta = channelDelta(expected.pixels[offset], actual.pixels[offset]);
        uint8_t const green_delta = channelDelta(expected.pixels[offset + 1U], actual.pixels[offset + 1U]);
        uint8_t const blue_delta = channelDelta(expected.pixels[offset + 2U], actual.pixels[offset + 2U]);
        uint8_t const alpha_delta = channelDelta(expected.pixels[offset + 3U], actual.pixels[offset + 3U]);
        uint8_t const pixel_delta = std::max({ red_delta, green_delta, blue_delta, alpha_delta });
        result.maximum_channel_delta = std::max(result.maximum_channel_delta, pixel_delta);
        if (pixel_delta <= policy.channel_tolerance) {
            continue;
        }
        if (result.mismatched_pixels == 0U) {
            uint64_t const pixel_index = offset / 4U;
            result.first_mismatch_x = static_cast<uint32_t>(pixel_index % expected.width);
            result.first_mismatch_y = static_cast<uint32_t>(pixel_index / expected.width);
        }
        ++result.mismatched_pixels;
    }
    result.matches = result.mismatched_pixels <= policy.max_mismatched_pixels;
    std::ostringstream diagnostic;
    diagnostic << "mismatched pixels=" << result.mismatched_pixels
        << ", allowed=" << policy.max_mismatched_pixels
        << ", max channel delta=" << static_cast<uint32_t>(result.maximum_channel_delta);
    if (result.mismatched_pixels > 0U) {
        diagnostic << ", first mismatch=(" << result.first_mismatch_x << ',' << result.first_mismatch_y << ')';
    }
    result.diagnostic = diagnostic.str();
    return result;
}

std::expected<std::monostate, std::string> writeMismatchArtifacts(
    Rgba8Image const& expected,
    Rgba8Image const& actual,
    ImageComparisonResult const& result,
    ImageComparisonPolicy const policy,
    std::filesystem::path const& output_directory,
    std::string const& artifact_stem
)
{
    if (std::expected<std::monostate, std::string> const actual_result = writePpm(
            actual,
            output_directory / (artifact_stem + "-actual.ppm"),
            "MC-AI-0052 actual capture; not an approved reference"
        );
        !actual_result.has_value()) {
        return std::unexpected(actual_result.error());
    }
    if (result.dimensions_match) {
        Rgba8Image const diff = makeDiffImage(expected, actual, policy.channel_tolerance);
        if (std::expected<std::monostate, std::string> const diff_result = writePpm(
                diff,
                output_directory / (artifact_stem + "-diff.ppm"),
                "MC-AI-0052 mismatch visualization; red marks changed pixels"
            );
            !diff_result.has_value()) {
            return std::unexpected(diff_result.error());
        }
    }
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) {
        return std::unexpected("cannot create diagnostics directory: " + error.message());
    }
    std::ofstream diagnostic{ output_directory / (artifact_stem + "-diagnostic.txt"), std::ios::trunc };
    if (!diagnostic) {
        return std::unexpected("cannot write mismatch diagnostic");
    }
    diagnostic << result.diagnostic << '\n';
    return std::monostate{};
}

} // namespace testsupport
