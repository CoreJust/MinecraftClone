#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace core::vk {

class SpirV final {
public:
    explicit SpirV(std::span<uint8_t const> data);

    static SpirV fromFile(std::string const& path);

    [[nodiscard]]
    std::span<uint32_t const> data() const noexcept {
        return m_words;
    }
private:
    std::vector<uint32_t> m_words;
};

} // namespace core::vk
