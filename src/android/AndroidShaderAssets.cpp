#include "AndroidShaderAssets.hpp"

#include <core/common/Assert.hpp>

#include <android/asset_manager.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace game_android {
namespace {

[[nodiscard]]
std::string assetPath(std::string_view const name)
{
    if (
        name.empty()
        || name == ".spv"
        || name.find_first_of("/\\") != std::string_view::npos
        || !name.ends_with(".spv")
    ) {
        throw std::invalid_argument("Android shader asset must be a bare .spv filename");
    }
    return "shaders/" + std::string{ name };
}

} // namespace

AndroidShaderAssets::AndroidShaderAssets(AAssetManager* const asset_manager) noexcept
    : m_asset_manager(asset_manager)
{
    ASSERT(m_asset_manager != nullptr, "Android shader assets require an asset manager");
}

core::vk::SpirV AndroidShaderAssets::load(std::string_view const name) const
{
    ASSERT(m_asset_manager != nullptr, "Android shader assets have no asset manager");
    std::string const path = assetPath(name);
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset{
        AAssetManager_open(m_asset_manager, path.c_str(), AASSET_MODE_BUFFER),
        AAsset_close,
    };
    if (!asset) {
        throw std::runtime_error("Missing Android shader asset: " + path);
    }

    using ByteStorage = std::vector<uint8_t>;

    int64_t const length = AAsset_getLength64(asset.get());
    uint64_t const asset_length = static_cast<uint64_t>(length);
    if (
        length <= 0
        || length % static_cast<int64_t>(sizeof(uint32_t)) != 0
        || asset_length > static_cast<uint64_t>(std::numeric_limits<ByteStorage::size_type>::max())
    ) {
        throw std::runtime_error("Invalid Android shader asset size: " + path);
    }

    ByteStorage bytes(static_cast<ByteStorage::size_type>(asset_length));
    uint64_t offset = 0;
    while (offset < asset_length) {
        uint64_t const remaining = asset_length - offset;
        int64_t const read = AAsset_read(
            asset.get(),
            bytes.data() + static_cast<ByteStorage::size_type>(offset),
            static_cast<ByteStorage::size_type>(remaining)
        );
        if (read <= 0 || static_cast<uint64_t>(read) > remaining) {
            throw std::runtime_error("Failed to read Android shader asset: " + path);
        }
        offset += static_cast<uint64_t>(read);
    }
    uint32_t const magic = static_cast<uint32_t>(bytes[0])
        | static_cast<uint32_t>(bytes[1]) << 8U
        | static_cast<uint32_t>(bytes[2]) << 16U
        | static_cast<uint32_t>(bytes[3]) << 24U;
    if (magic != 0x0723'0203U) {
        throw std::runtime_error("Invalid Android SPIR-V magic: " + path);
    }
    return core::vk::SpirV{ bytes };
}

} // namespace game_android
