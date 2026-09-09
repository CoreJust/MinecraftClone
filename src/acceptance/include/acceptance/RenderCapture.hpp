#pragma once

#include <acceptance/EvidenceJson.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace acceptance {

struct RenderCaptureOptions final {
    uint32_t width{ 1'920 };
    uint32_t height{ 1'080 };
    uint32_t max_resize_polls{ 60 };
    uint64_t max_frames{ 10 };
    std::chrono::seconds deadline{ 10 };
};

[[nodiscard]]
std::expected<RuntimeEvidence, std::string> captureRendererFrame(
    std::filesystem::path const& image_path,
    RenderCaptureOptions const& options = { }
);

} // namespace acceptance
