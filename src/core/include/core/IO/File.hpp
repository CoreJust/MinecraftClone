#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core {

[[nodiscard]]
std::optional<std::vector<uint8_t>> readFile(std::string const& path);
// True on success, false on failure
bool readFileTo(std::string const& path, std::span<uint8_t> const dst);

} // namespace core
