#include <core/IO/File.hpp>

#include <core/IO/Log.hpp>

#include <filesystem>

namespace core {

std::optional<std::vector<uint8_t>> readFile(std::string const& path) {
	size_t const file_size = std::filesystem::file_size(path);
    std::vector<uint8_t> result(file_size);
    if (!readFileTo(path, result)) {
        return std::nullopt;
    }
    return result;
}

bool readFileTo(std::string const& path, std::span<uint8_t> const dst) {
    if (!std::filesystem::exists(path)) {
        CORE_ERROR("No such file: {}", path);
        return false;
    }

    std::FILE* file = std::fopen(path.data(), "rb");
    if (file == nullptr) {
        CORE_ERROR("Failed to open file {}", path);
        return false;
    }

    if (dst.size() != std::fread(dst.data(), 1, dst.size(), file)) {
        CORE_ERROR("Failed to read file {}", path);
        return false;
    }

    return true;
}

} // namespace core
