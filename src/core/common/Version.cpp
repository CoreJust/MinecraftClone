#include "Version.hpp"

#include <tuple>

namespace core {
    bool Version::operator<(Version const& lhs) const noexcept {
        return std::tie(epoch, major, minor, patch) < std::tie(lhs.epoch, lhs.major, lhs.minor, lhs.patch);
    }
} // namespace core
