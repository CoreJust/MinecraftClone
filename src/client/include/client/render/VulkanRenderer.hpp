#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>
#include <core/window/Window.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace client {

struct PlayerRenderData final {
    uint32_t x = 0;
    uint32_t y = 0;
    std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

class VulkanRenderer final : core::NonCopyable, core::NonMovable {
public:
    explicit VulkanRenderer(core::Window const& window);
    ~VulkanRenderer();

    void render(std::span<PlayerRenderData const> const players);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace client
