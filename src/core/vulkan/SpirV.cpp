#include <core/vulkan/SpirV.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/File.hpp>

namespace core::vk {

SpirV::SpirV(std::vector<uint8_t>&& data) noexcept
    : m_data(data)
{
    ASSERT(m_data.size() % 4 == 0, "SPIR-V data size must be a factor of 4, but it isn't");
}

SpirV SpirV::fromFile(std::string const& path) {
    auto const bytes = core::readFile(path);
    ASSERT(bytes.has_value(), "Failed to read shader file: {}", path);

    return SpirV{ std::vector<uint8_t>(bytes->begin(), bytes->end()) };
}

} // namespace core::vk
