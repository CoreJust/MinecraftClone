#pragma once

#include <core/common/Version.hpp>

#include <string_view>

namespace shared {

constexpr std::string_view PROJECT_NAME{ "Minecraft Clone 2026" };

constexpr core::Version PROjECT_VERSION{ 
    .epoch = 0,
    .major = 1,
    .minor = 0,
    .patch = 3,
};

} // namespace shared
