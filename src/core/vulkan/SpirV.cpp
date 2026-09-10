#include <core/vulkan/SpirV.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/File.hpp>

#include <cstring>

namespace core::vk {
namespace {

[[nodiscard]]
std::vector<uint32_t> wordsFromBytes(std::span<uint8_t const> const bytes)
{
    ASSERT(
        bytes.size() % sizeof(uint32_t) == 0,
        "SPIR-V data size must be a multiple of 4, but it isn't"
    );

    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    if (!bytes.empty()) {
        std::memcpy(words.data(), bytes.data(), bytes.size());
    }
    return words;
}

} // namespace

SpirV::SpirV(std::span<uint8_t const> const data)
    : m_words(wordsFromBytes(data)) {}

SpirV SpirV::fromFile(std::string const& path) {
    auto const bytes = core::readFile(path);
    ASSERT(bytes.has_value(), "Failed to read shader file: {}", path);

    return SpirV{ *bytes };
}

} // namespace core::vk
