#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace client::detail {

[[nodiscard]] std::optional<std::vector<uint8_t>> encodeRgba8DibV5(
    uint32_t width,
    uint32_t height,
    std::span<uint8_t const> rgba8,
    std::string& error
);

} // namespace client::detail
