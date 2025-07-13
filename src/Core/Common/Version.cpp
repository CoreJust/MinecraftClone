#include "Version.hpp"
#include <tuple>

namespace core::common {
    bool Version::operator<(Version const& lhs) const noexcept {
        return std::tie(variant, major, minor, patch) < std::tie(lhs.variant, lhs.major, lhs.minor, lhs.patch);
    }
} // namespace core::common

