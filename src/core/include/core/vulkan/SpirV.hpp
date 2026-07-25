#pragma once

#include <core/common/SpanUtils.hpp>

#include <string>
#include <vector>

namespace core::vk {

class SpirV final {
public:
    explicit SpirV(std::vector<uint8_t>&& data) noexcept;

    static SpirV fromFile(std::string const& path);

    [[nodiscard]]
    std::span<uint32_t const> data() const noexcept {
        return asSpan<uint32_t>(m_data);
    }
private:
    std::vector<uint8_t> m_data;
};

} // namespace core::vk
