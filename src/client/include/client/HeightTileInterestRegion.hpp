#pragma once

#include <shared/net/Message.hpp>

#include <cstdint>
#include <optional>

namespace client {

class HeightTileInterestRegion final {
public:
    void set(shared::HeightTileKey center, uint32_t radius);
    void clear() noexcept;

    [[nodiscard]] bool contains(shared::HeightTileKey key) const;
    [[nodiscard]] std::optional<shared::HeightTileKey> center() const noexcept;
    [[nodiscard]] uint32_t radius() const noexcept;
    [[nodiscard]] uint32_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    constexpr bool operator==(HeightTileInterestRegion const&) const noexcept = default;

private:
    std::optional<shared::HeightTileKey> m_center;
    uint32_t m_radius = 0U;
    uint32_t m_size = 0U;
};

} // namespace client
