#pragma once

#include <filesystem>
#include <string_view>

namespace client {

[[nodiscard]]
std::filesystem::path shaderAssetPath(std::string_view name);

} // namespace client
