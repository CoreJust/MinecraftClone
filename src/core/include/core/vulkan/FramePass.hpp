#pragma once

#include <core/vulkan/FrameStructs.hpp>

#include <string>
#include <vector>

namespace core::vk {

enum class FrameResourceId : uint32_t { };
enum class FramePassId : uint32_t { };

struct FramePassOptions final {
    std::string name;
    std::vector<FramePassId> dependencies{ };
    std::vector<FrameResourceId> read_resources{ };
    std::vector<FrameResourceId> written_resources{ };
    RelativeViewport viewport{ };
    RelativeScissor scissor{ };
};

} // namespace core::vk
