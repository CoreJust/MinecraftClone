#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// DONT_CHECK INCLUDE_ORDER
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t MAX_ATLAS_RESOLUTION = 512U;
constexpr uint32_t MIN_ATLAS_RESOLUTION = 256U;
constexpr uint32_t MAX_SEEDS = 6U;
constexpr uint64_t MAX_OUTPUT_BYTES = uint64_t{512U} * 1024U * 1024U;
constexpr uint64_t WORLD_SIZE_BLOCKS = generation::WORLD_SIZE_BLOCKS;

using Clock = std::chrono::steady_clock;

enum class Layer : uint8_t {
    Height,
    Temperature,
    Water,
    TopMaterial,
};

struct RegionOptions final {
    uint32_t origin_x = 0U;
    uint32_t origin_y = 0U;
    uint32_t extent = 0U;
};

struct Options final {
    std::vector<uint64_t> seeds;
    std::vector<Layer> layers;
    uint32_t atlas_size = 0U;
    std::filesystem::path output;
    std::optional<RegionOptions> region;
};

struct Rgb final {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct RawFieldRecord final {
    std::string path;
    std::string dtype;
    std::string unit;
};

struct LayerRecord final {
    std::string name;
    std::string source;
    std::string color_mapping;
    std::string ppm_path;
    std::vector<RawFieldRecord> raw_fields;
    uint32_t width = 0U;
    uint32_t height = 0U;
    double display_min = 0.0;
    double display_max = 0.0;
};

struct RunTiming final {
    double simulation_ms = 0.0;
    double queries_ms = 0.0;
    double rasterization_ms = 0.0;
    double serialization_ms = 0.0;
};

struct UnsignedParseInput final {
    std::string_view text;
    std::string_view name;
};

struct ImageLayout final {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
};

struct HeightColorInput final {
    float value;
    float sea_level;
    float minimum;
    float maximum;
};

struct AtlasMemoryUsage final {
    uint64_t atlas_bytes;
    uint64_t peak_resident_bytes;
};

struct ManifestOutcome final {
    std::string_view status;
    std::string_view failure;
};

[[nodiscard]]
std::string_view usage() noexcept
{
    return "Usage: mc_world_map (--seed N | --seed-list N,N,...) --atlas-size 256|512 "
        "--layers height,temperature,water[,top-material] --output DIR [--region X,Y,EXTENT]\n";
}

[[nodiscard]]
uint64_t parseUnsigned(UnsignedParseInput const& input)
{
    uint64_t parsed = 0U;
    auto const [end, error] = std::from_chars(
        input.text.data(),
        input.text.data() + input.text.size(),
        parsed
    );
    if (error != std::errc{} || end != input.text.data() + input.text.size()) {
        throw std::runtime_error(std::string{input.name} + " must be an unsigned integer");
    }
    return parsed;
}

[[nodiscard]]
std::vector<uint64_t> parseSeedList(std::string_view const text)
{
    std::vector<uint64_t> seeds;
    uint64_t offset = 0U;
    while (offset <= text.size()) {
        uint64_t const separator = text.find(',', static_cast<std::string_view::size_type>(offset));
        uint64_t const end = separator == std::string_view::npos ? text.size() : separator;
        if (end == offset) {
            throw std::runtime_error("seed list contains an empty entry");
        }
        seeds.push_back(parseUnsigned(UnsignedParseInput{
            .text = text.substr(
                static_cast<std::string_view::size_type>(offset),
                static_cast<std::string_view::size_type>(end - offset)
            ),
            .name = "seed",
        }));
        if (seeds.size() > MAX_SEEDS) {
            throw std::runtime_error("seed list exceeds the six-seed output bound");
        }
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return seeds;
}

[[nodiscard]]
Layer parseLayer(std::string_view const name)
{
    if (name == "height") {
        return Layer::Height;
    }
    if (name == "temperature") {
        return Layer::Temperature;
    }
    if (name == "water") {
        return Layer::Water;
    }
    if (name == "top-material") {
        return Layer::TopMaterial;
    }
    throw std::runtime_error("unknown layer: " + std::string{name});
}

[[nodiscard]]
std::vector<Layer> parseLayers(std::string_view const text)
{
    std::vector<Layer> layers;
    uint64_t offset = 0U;
    while (offset <= text.size()) {
        uint64_t const separator = text.find(',', static_cast<std::string_view::size_type>(offset));
        uint64_t const end = separator == std::string_view::npos ? text.size() : separator;
        if (end == offset) {
            throw std::runtime_error("layer list contains an empty entry");
        }
        Layer const layer = parseLayer(text.substr(
            static_cast<std::string_view::size_type>(offset),
            static_cast<std::string_view::size_type>(end - offset)
        ));
        if (std::find(layers.begin(), layers.end(), layer) != layers.end()) {
            throw std::runtime_error("layer list contains a duplicate layer");
        }
        layers.push_back(layer);
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return layers;
}

[[nodiscard]]
RegionOptions parseRegion(std::string_view const text)
{
    std::array<uint64_t, 3U> values{};
    uint64_t offset = 0U;
    for (uint32_t index = 0U; index < static_cast<uint32_t>(values.size()); ++index) {
        uint64_t const separator = text.find(',', static_cast<std::string_view::size_type>(offset));
        uint64_t const end = separator == std::string_view::npos ? text.size() : separator;
        if ((index + 1U < static_cast<uint32_t>(values.size())) == (separator == std::string_view::npos)) {
            throw std::runtime_error("region must be X,Y,EXTENT");
        }
        values[index] = parseUnsigned(UnsignedParseInput{
            .text = text.substr(
                static_cast<std::string_view::size_type>(offset),
                static_cast<std::string_view::size_type>(end - offset)
            ),
            .name = "region coordinate",
        });
        offset = end + 1U;
    }
    if (std::any_of(values.begin(), values.end(), [](uint64_t const value) {
        return value > std::numeric_limits<uint32_t>::max();
    })) {
        throw std::runtime_error("region coordinate exceeds the supported range");
    }
    return {
        .origin_x = static_cast<uint32_t>(values[0U]),
        .origin_y = static_cast<uint32_t>(values[1U]),
        .extent = static_cast<uint32_t>(values[2U]),
    };
}

[[nodiscard]]
Options parseArguments(int const argc, char const* const* const argv)
{
    Options options{};
    bool has_seed = false;
    bool has_seed_list = false;
    bool has_atlas_size = false;
    bool has_layers = false;
    bool has_output = false;
    for (int index = 1; index < argc; ++index) {
        std::string_view const option = argv[index];
        if (option == "--seed" || option == "--seed-list" || option == "--atlas-size"
            || option == "--layers" || option == "--output" || option == "--region") {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string{option} + " requires a value");
            }
            std::string_view const value = argv[++index];
            if (option == "--seed") {
                if (has_seed || has_seed_list) {
                    throw std::runtime_error("choose exactly one of --seed and --seed-list");
                }
                options.seeds = {parseUnsigned(UnsignedParseInput{.text = value, .name = "seed"})};
                has_seed = true;
            } else if (option == "--seed-list") {
                if (has_seed || has_seed_list) {
                    throw std::runtime_error("choose exactly one of --seed and --seed-list");
                }
                options.seeds = parseSeedList(value);
                has_seed_list = true;
            } else if (option == "--atlas-size") {
                if (has_atlas_size) {
                    throw std::runtime_error("--atlas-size may be specified only once");
                }
                uint64_t const size = parseUnsigned(
                    UnsignedParseInput{.text = value, .name = "atlas size"}
                );
                if (size > std::numeric_limits<uint32_t>::max()) {
                    throw std::runtime_error("atlas size exceeds the supported range");
                }
                options.atlas_size = static_cast<uint32_t>(size);
                has_atlas_size = true;
            } else if (option == "--layers") {
                if (has_layers) {
                    throw std::runtime_error("--layers may be specified only once");
                }
                options.layers = parseLayers(value);
                has_layers = true;
            } else if (option == "--output") {
                if (has_output) {
                    throw std::runtime_error("--output may be specified only once");
                }
                options.output = std::filesystem::path{value};
                has_output = true;
            } else {
                if (options.region) {
                    throw std::runtime_error("--region may be specified only once");
                }
                options.region = parseRegion(value);
            }
        } else {
            throw std::runtime_error("unknown option: " + std::string{option});
        }
    }
    if (!has_seed && !has_seed_list) {
        throw std::runtime_error("one of --seed or --seed-list is required");
    }
    if (!has_atlas_size || !has_layers || !has_output) {
        throw std::runtime_error("--atlas-size, --layers, and --output are required");
    }
    if (options.seeds.empty()) {
        throw std::runtime_error("at least one seed is required");
    }
    if (options.atlas_size < MIN_ATLAS_RESOLUTION || options.atlas_size > MAX_ATLAS_RESOLUTION
        || (options.atlas_size & (options.atlas_size - 1U)) != 0U) {
        throw std::runtime_error("atlas size must be 256 or 512");
    }
    if (options.output.empty()) {
        throw std::runtime_error("output directory must not be empty");
    }
    bool const has_top_material = std::find(
        options.layers.begin(),
        options.layers.end(),
        Layer::TopMaterial
    ) != options.layers.end();
    if (has_top_material != options.region.has_value()) {
        throw std::runtime_error("--top-material requires --region, and --region requires --top-material");
    }
    if (options.region) {
        RegionOptions const& region = *options.region;
        bool const known_extent = std::find(
            shared::TERRAIN_REGION_EXTENTS.begin(),
            shared::TERRAIN_REGION_EXTENTS.end(),
            region.extent
        ) != shared::TERRAIN_REGION_EXTENTS.end();
        if (!known_extent || region.origin_x >= WORLD_SIZE_BLOCKS || region.origin_y >= WORLD_SIZE_BLOCKS) {
            throw std::runtime_error("region origin and extent must use the canonical world bounds");
        }
        if (region.origin_x % region.extent != 0U || region.origin_y % region.extent != 0U) {
            throw std::runtime_error("region origin must align to its extent");
        }
    }
    std::vector<uint64_t> sorted_seeds = options.seeds;
    std::sort(sorted_seeds.begin(), sorted_seeds.end());
    if (std::adjacent_find(sorted_seeds.begin(), sorted_seeds.end()) != sorted_seeds.end()) {
        throw std::runtime_error("seed list contains a duplicate seed");
    }
    return options;
}

[[nodiscard]]
std::string_view layerName(Layer const layer) noexcept
{
    switch (layer) {
        case Layer::Height:
            return "height";
        case Layer::Temperature:
            return "temperature";
        case Layer::Water:
            return "water";
        case Layer::TopMaterial:
            return "top-material";
    }
    return "unknown";
}

[[nodiscard]]
uint64_t checkedByteCount(ImageLayout const& layout)
{
    uint64_t const pixels = static_cast<uint64_t>(layout.width) * layout.height;
    if (pixels > std::numeric_limits<uint64_t>::max() / layout.channels) {
        throw std::runtime_error("requested output dimensions overflow the byte-size counter");
    }
    uint64_t const bytes = pixels * layout.channels;
    if (bytes > MAX_OUTPUT_BYTES) {
        throw std::runtime_error("requested output exceeds the 512 MiB file-size bound");
    }
    return bytes;
}

[[nodiscard]]
double milliseconds(Clock::duration const duration) noexcept
{
    return std::chrono::duration<double, std::milli>{duration}.count();
}

[[nodiscard]]
uint64_t peakResidentBytes() noexcept
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0U;
    }
    return static_cast<uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0U;
    }
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1'024U;
#endif
#endif
}

template <typename T>
[[nodiscard]]
uint64_t vectorBytes(std::vector<T> const& values) noexcept
{
    return static_cast<uint64_t>(values.capacity()) * sizeof(T);
}

[[nodiscard]]
uint64_t atlasStorageBytes(generation::GlobalAtlas const& atlas) noexcept
{
    return vectorBytes(atlas.elevation)
        + vectorBytes(atlas.tectonic_elevation)
        + vectorBytes(atlas.filled)
        + vectorBytes(atlas.water_level)
        + vectorBytes(atlas.flow_accumulation)
        + vectorBytes(atlas.downstream)
        + vectorBytes(atlas.lake_id)
        + vectorBytes(atlas.outlet_id)
        + vectorBytes(atlas.lake_mask)
        + vectorBytes(atlas.river_mask)
        + vectorBytes(atlas.ocean_mask)
        + vectorBytes(atlas.fjord_candidate_mask)
        + vectorBytes(atlas.harbour_candidate_mask)
        + vectorBytes(atlas.temperature)
        + vectorBytes(atlas.precipitation)
        + vectorBytes(atlas.moisture)
        + vectorBytes(atlas.wind_u)
        + vectorBytes(atlas.wind_v)
        + vectorBytes(atlas.current_u)
        + vectorBytes(atlas.current_v)
        + vectorBytes(atlas.climate_zone)
        + vectorBytes(atlas.snow_mask)
        + vectorBytes(atlas.uplift)
        + vectorBytes(atlas.island_arc_uplift)
        + vectorBytes(atlas.hotspot)
        + vectorBytes(atlas.subduction)
        + vectorBytes(atlas.divergence)
        + vectorBytes(atlas.continental)
        + vectorBytes(atlas.plate_vx)
        + vectorBytes(atlas.plate_vy)
        + vectorBytes(atlas.plate_id);
}

[[nodiscard]]
std::pair<float, float> finiteRange(std::span<float const> values, std::string_view const name)
{
    if (values.empty()) {
        throw std::runtime_error(std::string{name} + " field is empty");
    }
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    for (float const value : values) {
        if (!std::isfinite(value)) {
            throw std::runtime_error(std::string{name} + " field contains a non-finite value");
        }
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    return {minimum, maximum};
}

[[nodiscard]]
float normalize(float const value, float const minimum, float const maximum) noexcept
{
    if (maximum <= minimum) {
        return 0.5F;
    }
    return std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
}

[[nodiscard]]
uint8_t channel(float const value) noexcept
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
}

[[nodiscard]]
Rgb mix(Rgb const first, Rgb const second, float const amount) noexcept
{
    return {
        channel(std::fma(
            static_cast<float>(second.red) - static_cast<float>(first.red),
            amount,
            static_cast<float>(first.red)
        )),
        channel(std::fma(
            static_cast<float>(second.green) - static_cast<float>(first.green),
            amount,
            static_cast<float>(first.green)
        )),
        channel(std::fma(
            static_cast<float>(second.blue) - static_cast<float>(first.blue),
            amount,
            static_cast<float>(first.blue)
        )),
    };
}

[[nodiscard]]
Rgb heightColor(HeightColorInput const& input) noexcept
{
    if (input.value < input.sea_level) {
        float const depth = normalize(input.value, input.minimum, input.sea_level);
        return mix({8U, 34U, 70U}, {62U, 146U, 177U}, depth);
    }
    float const elevation = normalize(input.value, input.sea_level, input.maximum);
    if (elevation < 0.7F) {
        return mix({43U, 91U, 52U}, {155U, 139U, 88U}, elevation / 0.7F);
    }
    return mix({155U, 139U, 88U}, {239U, 241U, 233U}, (elevation - 0.7F) / 0.3F);
}

[[nodiscard]]
Rgb temperatureColor(float const value, float const minimum, float const maximum) noexcept
{
    float const temperature = normalize(value, minimum, maximum);
    if (temperature < 0.5F) {
        return mix({28U, 67U, 149U}, {243U, 238U, 207U}, temperature * 2.0F);
    }
    return mix({243U, 238U, 207U}, {177U, 48U, 36U}, (temperature - 0.5F) * 2.0F);
}

[[nodiscard]]
Rgb waterColor(
    uint32_t const index,
    generation::GlobalAtlas const& atlas
) noexcept
{
    if (atlas.river_mask[index] != 0U) {
        return {73U, 190U, 206U};
    }
    if (atlas.lake_mask[index] != 0U) {
        return {42U, 131U, 179U};
    }
    if (atlas.ocean_mask[index] != 0U) {
        return {19U, 66U, 125U};
    }
    return {83U, 91U, 69U};
}

[[nodiscard]]
Rgb materialColor(shared::Block const material)
{
    constexpr std::array<Rgb, 7U> palette = {
        Rgb{0U, 0U, 0U},
        Rgb{99U, 96U, 91U},
        Rgb{87U, 142U, 56U},
        Rgb{132U, 91U, 55U},
        Rgb{55U, 53U, 58U},
        Rgb{235U, 240U, 242U},
        Rgb{119U, 177U, 203U},
    };
    if (!shared::isValidBlock(material)) {
        throw std::runtime_error("regional sampler returned an invalid top-material ID");
    }
    return palette[static_cast<uint8_t>(material)];
}

void writeBytes(std::filesystem::path const& path, std::span<uint8_t const> bytes)
{
    if (bytes.size() > MAX_OUTPUT_BYTES) {
        throw std::runtime_error("output file exceeds the 512 MiB size bound");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("cannot open output file: " + path.string());
    }
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        throw std::runtime_error("failed while writing output file: " + path.string());
    }
}

void writePpm(
    std::filesystem::path const& path,
    uint32_t const width,
    uint32_t const height,
    std::span<Rgb const> pixels
)
{
    uint64_t const bytes = checkedByteCount(ImageLayout{.width = width, .height = height, .channels = 3U});
    if (pixels.size() != static_cast<uint64_t>(width) * height) {
        throw std::runtime_error("PPM pixel count does not match its dimensions");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("cannot open PPM output: " + path.string());
    }
    file << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<uint8_t> packed_pixels(static_cast<uint32_t>(bytes));
    for (uint32_t index = 0U; index < static_cast<uint32_t>(pixels.size()); ++index) {
        std::vector<uint8_t>::size_type const packed_index =
            static_cast<std::vector<uint8_t>::size_type>(index) * 3U;
        packed_pixels[packed_index] = pixels[index].red;
        packed_pixels[packed_index + 1U] = pixels[index].green;
        packed_pixels[packed_index + 2U] = pixels[index].blue;
    }
    file.write(
        reinterpret_cast<char const*>(packed_pixels.data()),
        static_cast<std::streamsize>(packed_pixels.size())
    );
    file.close();
    if (!file || bytes != static_cast<uint64_t>(packed_pixels.size())) {
        throw std::runtime_error("failed while writing PPM output: " + path.string());
    }
}

[[nodiscard]]
std::vector<uint8_t> encodeFloat32(std::span<float const> values)
{
    uint64_t const byte_count = static_cast<uint64_t>(values.size()) * sizeof(float);
    if (byte_count > MAX_OUTPUT_BYTES) {
        throw std::runtime_error("raw field exceeds the 512 MiB size bound");
    }
    std::vector<uint8_t> bytes(static_cast<uint32_t>(byte_count));
    for (uint32_t index = 0U; index < static_cast<uint32_t>(values.size()); ++index) {
        uint32_t const bits = std::bit_cast<uint32_t>(values[index]);
        std::vector<uint8_t>::size_type const offset =
            static_cast<std::vector<uint8_t>::size_type>(index) * sizeof(float);
        bytes[offset] = static_cast<uint8_t>(bits);
        bytes[offset + 1U] = static_cast<uint8_t>(bits >> 8U);
        bytes[offset + 2U] = static_cast<uint8_t>(bits >> 16U);
        bytes[offset + 3U] = static_cast<uint8_t>(bits >> 24U);
    }
    return bytes;
}

[[nodiscard]]
std::vector<uint8_t> encodeUint16(std::span<uint16_t const> values)
{
    uint64_t const byte_count = static_cast<uint64_t>(values.size()) * sizeof(uint16_t);
    if (byte_count > MAX_OUTPUT_BYTES) {
        throw std::runtime_error("raw field exceeds the 512 MiB size bound");
    }
    std::vector<uint8_t> bytes(static_cast<uint32_t>(byte_count));
    for (uint32_t index = 0U; index < static_cast<uint32_t>(values.size()); ++index) {
        std::vector<uint8_t>::size_type const offset =
            static_cast<std::vector<uint8_t>::size_type>(index) * sizeof(uint16_t);
        bytes[offset] = static_cast<uint8_t>(values[index]);
        bytes[offset + 1U] = static_cast<uint8_t>(values[index] >> 8U);
    }
    return bytes;
}

[[nodiscard]]
std::string jsonEscape(std::string_view const value)
{
    std::string result;
    result.reserve(value.size() + 2U);
    for (char const character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        if (character == '\n') {
            result.append("\\n");
        } else if (character == '\r') {
            result.append("\\r");
        } else if (character == '\t') {
            result.append("\\t");
        } else if (static_cast<unsigned char>(character) >= 0x20U) {
            result.push_back(character);
        }
    }
    return result;
}

void accountFile(
    std::filesystem::path const& file,
    uint64_t& output_bytes
)
{
    std::error_code error;
    uint64_t const bytes = std::filesystem::file_size(file, error);
    if (error) {
        throw std::runtime_error("cannot measure output file: " + file.string());
    }
    if (bytes > MAX_OUTPUT_BYTES - output_bytes) {
        throw std::runtime_error("run output exceeds the 512 MiB total-size bound");
    }
    output_bytes += bytes;
}

void writeFloatField(
    std::filesystem::path const& output,
    std::string const& relative,
    std::span<float const> values,
    std::string const& unit,
    LayerRecord& record,
    uint64_t& output_bytes,
    RunTiming& timing
)
{
    Clock::time_point const started = Clock::now();
    std::vector<uint8_t> const bytes = encodeFloat32(values);
    std::filesystem::path const path = output / relative;
    writeBytes(path, bytes);
    accountFile(path, output_bytes);
    record.raw_fields.push_back({relative, "float32", unit});
    timing.serialization_ms += milliseconds(Clock::now() - started);
}

void writeMaskField(
    std::filesystem::path const& output,
    std::string const& relative,
    std::span<uint8_t const> values,
    LayerRecord& record,
    uint64_t& output_bytes,
    RunTiming& timing
)
{
    Clock::time_point const started = Clock::now();
    std::filesystem::path const path = output / relative;
    writeBytes(path, values);
    accountFile(path, output_bytes);
    record.raw_fields.push_back({relative, "uint8", "mask"});
    timing.serialization_ms += milliseconds(Clock::now() - started);
}

void writeHeightField(
    std::filesystem::path const& output,
    std::string const& relative,
    std::span<uint16_t const> values,
    LayerRecord& record,
    uint64_t& output_bytes,
    RunTiming& timing
)
{
    Clock::time_point const started = Clock::now();
    std::vector<uint8_t> const bytes = encodeUint16(values);
    std::filesystem::path const path = output / relative;
    writeBytes(path, bytes);
    accountFile(path, output_bytes);
    record.raw_fields.push_back({relative, "uint16", "block_z"});
    timing.serialization_ms += milliseconds(Clock::now() - started);
}

void writeLayerPpm(
    std::filesystem::path const& output,
    LayerRecord const& record,
    std::span<Rgb const> pixels,
    uint64_t& output_bytes,
    RunTiming& timing
)
{
    Clock::time_point const started = Clock::now();
    std::filesystem::path const path = output / record.ppm_path;
    writePpm(path, record.width, record.height, pixels);
    accountFile(path, output_bytes);
    timing.serialization_ms += milliseconds(Clock::now() - started);
}

void ensureFieldDirectory(std::filesystem::path const& output)
{
    std::filesystem::path const fields = output / "fields";
    std::error_code error;
    if (std::filesystem::create_directory(fields, error) || (!error && std::filesystem::is_directory(fields))) {
        return;
    }
    throw std::runtime_error("cannot create raw-field directory: " + fields.string());
}

[[nodiscard]]
LayerRecord exportAtlasLayer(
    Layer const layer,
    generation::GlobalAtlas const& atlas,
    std::filesystem::path const& output,
    uint64_t& output_bytes,
    RunTiming& timing
)
{
    uint32_t const resolution = atlas.configuration.resolution;
    uint32_t const expected_count = resolution * resolution;
    LayerRecord record{
        .name = std::string{layerName(layer)},
        .source = layer == Layer::Height ? "GlobalAtlas.elevation"
            : layer == Layer::Temperature ? "GlobalAtlas.temperature"
            : "GlobalAtlas.water_level and water masks",
        .color_mapping = layer == Layer::Height ? "height diagnostic gradient"
            : layer == Layer::Temperature ? "temperature diagnostic gradient"
            : "ocean, lake, river, and land diagnostic classes",
        .ppm_path = std::string{layerName(layer)} + ".ppm",
        .raw_fields = {},
        .width = resolution,
        .height = resolution,
    };
    std::vector<Rgb> pixels;
    pixels.reserve(expected_count);
    ensureFieldDirectory(output);
    if (layer == Layer::Height) {
        if (atlas.elevation.size() != expected_count) {
            throw std::runtime_error("atlas height dimensions do not match the requested resolution");
        }
        auto const [minimum, maximum] = finiteRange(atlas.elevation, "height");
        record.display_min = minimum;
        record.display_max = maximum;
        Clock::time_point const started = Clock::now();
        for (float const value : atlas.elevation) {
            pixels.push_back(heightColor(HeightColorInput{
                .value = value,
                .sea_level = atlas.sea_level,
                .minimum = minimum,
                .maximum = maximum,
            }));
        }
        timing.rasterization_ms += milliseconds(Clock::now() - started);
        writeLayerPpm(output, record, pixels, output_bytes, timing);
        writeFloatField(
            output,
            "fields/height.f32le",
            atlas.elevation,
            "geological_km",
            record,
            output_bytes,
            timing
        );
    } else if (layer == Layer::Temperature) {
        if (atlas.temperature.size() != expected_count) {
            throw std::runtime_error("atlas temperature dimensions do not match the requested resolution");
        }
        auto const [minimum, maximum] = finiteRange(atlas.temperature, "temperature");
        record.display_min = minimum;
        record.display_max = maximum;
        Clock::time_point const started = Clock::now();
        for (float const value : atlas.temperature) {
            pixels.push_back(temperatureColor(value, minimum, maximum));
        }
        timing.rasterization_ms += milliseconds(Clock::now() - started);
        writeLayerPpm(output, record, pixels, output_bytes, timing);
        writeFloatField(
            output,
            "fields/temperature.f32le",
            atlas.temperature,
            "degrees_celsius",
            record,
            output_bytes,
            timing
        );
    } else if (layer == Layer::Water) {
        if (atlas.water_level.size() != expected_count
            || atlas.ocean_mask.size() != expected_count
            || atlas.lake_mask.size() != expected_count
            || atlas.river_mask.size() != expected_count) {
            throw std::runtime_error("atlas water fields do not match the requested resolution");
        }
        auto const [minimum, maximum] = finiteRange(atlas.water_level, "water level");
        record.display_min = minimum;
        record.display_max = maximum;
        Clock::time_point const started = Clock::now();
        for (uint32_t index = 0U; index < expected_count; ++index) {
            pixels.push_back(waterColor(index, atlas));
        }
        timing.rasterization_ms += milliseconds(Clock::now() - started);
        writeLayerPpm(output, record, pixels, output_bytes, timing);
        writeFloatField(
            output,
            "fields/water.f32le",
            atlas.water_level,
            "geological_km",
            record,
            output_bytes,
            timing
        );
        writeMaskField(output, "fields/ocean-mask.u8", atlas.ocean_mask, record, output_bytes, timing);
        writeMaskField(output, "fields/lake-mask.u8", atlas.lake_mask, record, output_bytes, timing);
        writeMaskField(output, "fields/river-mask.u8", atlas.river_mask, record, output_bytes, timing);
    } else {
        throw std::runtime_error("top-material layer requires canonical regional sampling");
    }
    return record;
}

[[nodiscard]]
LayerRecord exportTopMaterialLayer(
    std::span<shared::RegionalSample const> samples,
    uint32_t const side_length,
    std::filesystem::path const& output,
    uint64_t& output_bytes,
    RunTiming& timing
)
{
    uint64_t const expected = checkedByteCount(
        ImageLayout{.width = side_length, .height = side_length, .channels = 1U}
    );
    if (samples.size() != expected) {
        throw std::runtime_error("regional sample count does not match its dimensions");
    }
    LayerRecord record{
        .name = "top-material",
        .source = "TerrainWorld::sampleRegion().top_material",
        .color_mapping = "material_palette RGB",
        .ppm_path = "top-material.ppm",
        .raw_fields = {},
        .width = side_length,
        .height = side_length,
    };
    std::vector<Rgb> pixels;
    std::vector<uint8_t> material_ids;
    std::vector<uint16_t> heights;
    pixels.reserve(samples.size());
    material_ids.reserve(samples.size());
    heights.reserve(samples.size());
    ensureFieldDirectory(output);
    Clock::time_point const started = Clock::now();
    for (shared::RegionalSample const& sample : samples) {
        pixels.push_back(materialColor(sample.top_material));
        material_ids.push_back(static_cast<uint8_t>(sample.top_material));
        heights.push_back(sample.highest_solid_z);
    }
    auto const [minimum, maximum] = std::minmax_element(material_ids.begin(), material_ids.end());
    record.display_min = *minimum;
    record.display_max = *maximum;
    timing.rasterization_ms += milliseconds(Clock::now() - started);
    writeLayerPpm(output, record, pixels, output_bytes, timing);
    writeMaskField(output, "fields/top-material.u8", material_ids, record, output_bytes, timing);
    writeHeightField(output, "fields/top-height.u16le", heights, record, output_bytes, timing);
    record.raw_fields[0U].unit = "block_material_id";
    return record;
}

[[nodiscard]]
std::string stageName(generation::GenerationStage const stage)
{
    switch (stage) {
        case generation::GenerationStage::Tectonics:
            return "tectonics";
        case generation::GenerationStage::CoastalElaboration:
            return "coastal_elaboration";
        case generation::GenerationStage::InitialDrainage:
            return "initial_drainage";
        case generation::GenerationStage::ProvisionalClimate:
            return "provisional_climate";
        case generation::GenerationStage::TerrainErosion:
            return "terrain_erosion";
        case generation::GenerationStage::FinalClimate:
            return "final_climate";
        case generation::GenerationStage::FinalDrainage:
            return "final_drainage";
        case generation::GenerationStage::Complete:
            return "complete";
    }
    return "unknown";
}

void writeMetadata(
    std::filesystem::path const& path,
    generation::GenerationConfiguration const& configuration,
    std::string_view const configuration_identity,
    std::span<LayerRecord const> layers,
    std::optional<RegionOptions> const& region,
    RunTiming const& timing,
    float const sea_level,
    AtlasMemoryUsage const& memory_usage,
    uint64_t& output_bytes
)
{
    std::ostringstream json;
    json.precision(9);
    json << "{\n"
        << "  \"schema\": \"mc-world-map/1\",\n"
        << "  \"terrain_version\": " << generation::TERRAIN_VERSION << ",\n"
        << "  \"model_revision\": " << generation::MODEL_REVISION << ",\n"
        << "  \"configuration\": {\n"
        << "    \"seed\": " << configuration.seed << ",\n"
        << "    \"resolution\": " << configuration.resolution << ",\n"
        << "    \"epochs\": " << configuration.epochs << ",\n"
        << "    \"erosion_steps\": " << configuration.erosion_steps << ",\n"
        << "    \"initial_climate_iterations\": " << configuration.initial_climate_iterations << ",\n"
        << "    \"climate_iterations\": " << configuration.climate_iterations << ",\n"
        << "    \"currents\": " << (configuration.currents ? "true" : "false") << ",\n"
        << "    \"wind_sign\": " << static_cast<int>(configuration.wind_sign) << ",\n"
        << "    \"coastal_elaboration\": "
        << (configuration.coastal_elaboration ? "true" : "false") << "\n"
        << "  },\n"
        << "  \"configuration_identity\": \"" << jsonEscape(configuration_identity) << "\",\n"
        << "  \"source_identity\": {\n"
        << "    \"kind\": \"terrain-v2-python-source-map\",\n"
        << "    \"sha256\": \"" << generation::MODEL_SOURCE_MAP_SHA256 << "\"\n"
        << "  },\n"
        << "  \"atlas\": {\n"
        << "    \"width\": " << configuration.resolution << ",\n"
        << "    \"height\": " << configuration.resolution << ",\n"
        << "    \"layout\": \"row-major-yx\",\n"
        << "    \"sea_level_geological_km\": " << sea_level << "\n"
        << "  },\n"
        << "  \"region\": ";
    if (region) {
        json << "{\"origin_x\": " << region->origin_x
            << ", \"origin_y\": " << region->origin_y
            << ", \"extent_blocks\": " << region->extent
            << ", \"sample_side\": " << shared::TERRAIN_REGION_NODE_SIDE_LENGTH << "},\n";
    } else {
        json << "null,\n";
    }
    json << "  \"layers\": [\n";
    for (uint32_t index = 0U; index < static_cast<uint32_t>(layers.size()); ++index) {
        LayerRecord const& layer = layers[index];
        json << "    {\"name\": \"" << jsonEscape(layer.name)
            << "\", \"kind\": \""
            << (layer.name == "top-material" ? "canonical_surface" : "diagnostic_atlas")
            << "\", \"source\": \"" << jsonEscape(layer.source)
            << "\", \"color_mapping\": \"" << jsonEscape(layer.color_mapping)
            << "\", \"ppm\": \"" << jsonEscape(layer.ppm_path)
            << "\", \"shape\": [" << layer.height << ", " << layer.width << "]"
            << ", \"source_field_range\": [" << layer.display_min << ", " << layer.display_max << "]"
            << ", \"raw_fields\": [";
        for (uint32_t raw_index = 0U; raw_index < static_cast<uint32_t>(layer.raw_fields.size()); ++raw_index) {
            RawFieldRecord const& raw = layer.raw_fields[raw_index];
            if (raw_index != 0U) {
                json << ", ";
            }
            json << "{\"path\": \"" << jsonEscape(raw.path)
                << "\", \"dtype\": \"" << jsonEscape(raw.dtype)
                << "\", \"byte_order\": \"little\", \"unit\": \""
                << jsonEscape(raw.unit) << "\"}";
        }
        json << "]}" << (index + 1U == layers.size() ? "\n" : ",\n");
    }
    json << "  ],\n"
        << "  \"material_palette\": {\n"
        << "    \"0\": {\"name\": \"Air\", \"rgb\": [0, 0, 0]},\n"
        << "    \"1\": {\"name\": \"Stone\", \"rgb\": [99, 96, 91]},\n"
        << "    \"2\": {\"name\": \"Grass\", \"rgb\": [87, 142, 56]},\n"
        << "    \"3\": {\"name\": \"Dirt\", \"rgb\": [132, 91, 55]},\n"
        << "    \"4\": {\"name\": \"Bedrock\", \"rgb\": [55, 53, 58]},\n"
        << "    \"5\": {\"name\": \"Snow\", \"rgb\": [235, 240, 242]},\n"
        << "    \"6\": {\"name\": \"Ice\", \"rgb\": [119, 177, 203]}\n"
        << "  },\n"
        << "  \"timing_ms\": {\n"
        << "    \"simulation\": " << timing.simulation_ms << ",\n"
        << "    \"queries\": " << timing.queries_ms << ",\n"
        << "    \"rasterization\": " << timing.rasterization_ms << ",\n"
        << "    \"serialization\": " << timing.serialization_ms << "\n"
        << "  },\n"
        << "  \"timing_scope\": \"serialization includes PPM/raw fields and excludes this metadata file\",\n"
        << "  \"memory_bytes\": {\n"
        << "    \"atlas_storage\": " << memory_usage.atlas_bytes << ",\n"
        << "    \"peak_resident\": " << memory_usage.peak_resident_bytes << "\n"
        << "  },\n"
        << "  \"checks\": {\n"
        << "    \"configuration_valid\": true,\n"
        << "    \"atlas_dimensions\": true,\n"
        << "    \"fields_finite\": true,\n"
        << "    \"regional_samples_valid\": true,\n"
        << "    \"output_sizes_bounded\": true\n"
        << "  }\n"
        << "}\n";
    std::string const text = json.str();
    if (text.size() > MAX_OUTPUT_BYTES) {
        throw std::runtime_error("metadata exceeds the 512 MiB size bound");
    }
    std::vector<uint8_t> const bytes(text.begin(), text.end());
    writeBytes(path, bytes);
    accountFile(path, output_bytes);
}

[[nodiscard]]
std::string seedProgress(generation::GenerationProgress const& progress, uint64_t const seed)
{
    std::ostringstream message;
    message << "seed=" << seed << " stage=" << stageName(progress.stage)
        << " completed=" << progress.completed << '/' << progress.total;
    return message.str();
}

void writeManifest(
    std::filesystem::path const& output,
    std::span<uint64_t const> requested,
    std::span<uint64_t const> completed,
    ManifestOutcome const& outcome,
    uint32_t const atlas_size,
    std::span<Layer const> layers,
    uint64_t& output_bytes
)
{
    std::ostringstream json;
    json << "{\n  \"schema\": \"mc-world-map/1\",\n"
        << "  \"status\": \"" << jsonEscape(outcome.status) << "\",\n"
        << "  \"atlas_size\": " << atlas_size << ",\n"
        << "  \"seeds\": [";
    for (uint32_t index = 0U; index < static_cast<uint32_t>(requested.size()); ++index) {
        if (index != 0U) {
            json << ", ";
        }
        json << requested[index];
    }
    json << "],\n  \"completed_seeds\": [";
    for (uint32_t index = 0U; index < static_cast<uint32_t>(completed.size()); ++index) {
        if (index != 0U) {
            json << ", ";
        }
        json << completed[index];
    }
    json << "],\n  \"layers\": [";
    for (uint32_t index = 0U; index < static_cast<uint32_t>(layers.size()); ++index) {
        if (index != 0U) {
            json << ", ";
        }
        json << '"' << layerName(layers[index]) << '"';
    }
    json << "],\n  \"failure\": ";
    if (outcome.failure.empty()) {
        json << "null\n";
    } else {
        json << '"' << jsonEscape(outcome.failure) << "\"\n";
    }
    json << "}\n";
    std::string const text = json.str();
    std::vector<uint8_t> const bytes(text.begin(), text.end());
    std::filesystem::path const path = output / "manifest.json";
    writeBytes(path, bytes);
    accountFile(path, output_bytes);
}

[[nodiscard]]
std::vector<shared::RegionalSample> queryRegion(
    std::shared_ptr<generation::GlobalAtlas const> const& atlas,
    RegionOptions const& region,
    double& query_ms
)
{
    Clock::time_point const started = Clock::now();
    shared::TerrainWorld const world{atlas};
    std::shared_ptr<shared::RegionalArtifact const> const artifact = world.prepareRegion(
        {
            .origin_x = region.origin_x,
            .origin_y = region.origin_y,
            .extent = region.extent,
        }
    );
    if (!artifact) {
        throw std::runtime_error("canonical regional sampler returned no artifact");
    }
    uint32_t const side_length = shared::TERRAIN_REGION_NODE_SIDE_LENGTH;
    double const spacing = static_cast<double>(region.extent) / (side_length - 1U);
    std::vector<shared::RegionalSample> samples;
    samples.reserve(shared::TERRAIN_REGION_NODE_COUNT);
    for (uint32_t y = 0U; y < side_length; ++y) {
        for (uint32_t x = 0U; x < side_length; ++x) {
            double const world_x = static_cast<double>(region.origin_x) + x * spacing;
            double const world_y = static_cast<double>(region.origin_y) + y * spacing;
            shared::RegionalSample const sample = world.sampleRegion(*artifact, world_x, world_y);
            if (!std::isfinite(sample.geological_elevation_km)
                || !std::isfinite(sample.temperature_celsius)
                || !std::isfinite(sample.precipitation)
                || sample.highest_solid_z == 0U
                || sample.highest_solid_z > generation::MAX_HEIGHT
                || !shared::isValidBlock(sample.top_material)) {
                throw std::runtime_error("canonical regional sampler returned an invalid top-column sample");
            }
            samples.push_back(sample);
        }
    }
    query_ms += milliseconds(Clock::now() - started);
    return samples;
}

void processSeed(
    uint64_t const seed,
    Options const& options,
    std::filesystem::path const& output_root,
    uint64_t& output_bytes
)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = seed;
    configuration.resolution = options.atlas_size;
    RunTiming timing{};
    Clock::time_point const simulation_started = Clock::now();
    generation::ProgressCallback const progress = [seed, last_report = Clock::time_point{},
        last_stage = std::optional<generation::GenerationStage>{}](
        generation::GenerationProgress const& update
    ) mutable {
        Clock::time_point const now = Clock::now();
        if (last_stage != update.stage || now - last_report >= std::chrono::seconds{5}) {
            std::string const message = seedProgress(update, seed);
            std::fprintf(stderr, "%s\n", message.c_str());
            last_stage = update.stage;
            last_report = now;
        }
    };
    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
        configuration,
        {},
        progress
    );
    timing.simulation_ms = milliseconds(Clock::now() - simulation_started);
    if (!atlas || atlas->configuration != configuration) {
        throw std::runtime_error("atlas producer returned an invalid configuration");
    }
    uint32_t const atlas_cells = options.atlas_size * options.atlas_size;
    if (atlas->elevation.size() != atlas_cells || atlas->temperature.size() != atlas_cells
        || atlas->water_level.size() != atlas_cells || atlas->ocean_mask.size() != atlas_cells
        || atlas->lake_mask.size() != atlas_cells || atlas->river_mask.size() != atlas_cells) {
        throw std::runtime_error("atlas producer returned fields with invalid dimensions");
    }

    std::vector<shared::RegionalSample> regional_samples;
    if (options.region) {
        regional_samples = queryRegion(atlas, *options.region, timing.queries_ms);
    }

    std::filesystem::path const seed_output = output_root / ("seed-" + std::to_string(seed));
    std::error_code error;
    if (!std::filesystem::create_directory(seed_output, error) || error) {
        throw std::runtime_error("cannot create seed output directory: " + error.message());
    }
    std::vector<LayerRecord> records;
    records.reserve(options.layers.size());
    for (Layer const layer : options.layers) {
        if (layer == Layer::TopMaterial) {
            records.push_back(exportTopMaterialLayer(
                regional_samples,
                shared::TERRAIN_REGION_NODE_SIDE_LENGTH,
                seed_output,
                output_bytes,
                timing
            ));
        } else {
            records.push_back(exportAtlasLayer(layer, *atlas, seed_output, output_bytes, timing));
        }
    }
    uint64_t const peak_rss = peakResidentBytes();
    writeMetadata(
        seed_output / "metadata.json",
        configuration,
        generation::configurationIdentity(configuration),
        records,
        options.region,
        timing,
        atlas->sea_level,
        AtlasMemoryUsage{.atlas_bytes = atlasStorageBytes(*atlas), .peak_resident_bytes = peak_rss},
        output_bytes
    );
}

} // namespace

int main(int const argc, char const* const* const argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::string_view const help = usage();
        std::fwrite(help.data(), sizeof(char), help.size(), stdout);
        return 0;
    }
    Options options{};
    try {
        options = parseArguments(argc, argv);
        std::filesystem::path const parent = options.output.has_parent_path()
            ? options.output.parent_path()
            : std::filesystem::current_path();
        if (!std::filesystem::is_directory(parent)) {
            throw std::runtime_error("output parent must already be a directory");
        }
        if (std::filesystem::exists(options.output)) {
            throw std::runtime_error("output path already exists; refusing to overwrite it");
        }
        std::error_code error;
        if (!std::filesystem::create_directory(options.output, error) || error) {
            throw std::runtime_error("cannot create output directory: " + error.message());
        }
        uint64_t output_bytes = 0U;
        std::vector<uint64_t> completed;
        writeManifest(
            options.output,
            options.seeds,
            completed,
            ManifestOutcome{.status = "running", .failure = {}},
            options.atlas_size,
            options.layers,
            output_bytes
        );
        for (uint64_t const seed : options.seeds) {
            try {
                processSeed(seed, options, options.output, output_bytes);
                completed.push_back(seed);
                writeManifest(
                    options.output,
                    options.seeds,
                    completed,
                    ManifestOutcome{.status = "running", .failure = {}},
                    options.atlas_size,
                    options.layers,
                    output_bytes
                );
            } catch (std::exception const& exception) {
                writeManifest(
                    options.output,
                    options.seeds,
                    completed,
                    ManifestOutcome{.status = "failed", .failure = exception.what()},
                    options.atlas_size,
                    options.layers,
                    output_bytes
                );
                throw;
            }
        }
        writeManifest(
            options.output,
            options.seeds,
            completed,
            ManifestOutcome{.status = "complete", .failure = {}},
            options.atlas_size,
            options.layers,
            output_bytes
        );
        std::fprintf(stdout, "exported %zu world map(s) to %s\n", completed.size(), options.output.string().c_str());
        return 0;
    } catch (std::exception const& exception) {
        std::fprintf(stderr, "mc_world_map: %s\n", exception.what());
        std::string_view const help = usage();
        std::fwrite(help.data(), sizeof(char), help.size(), stderr);
        return 1;
    }
}
