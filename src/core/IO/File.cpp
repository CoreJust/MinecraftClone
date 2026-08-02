#include <core/IO/File.hpp>

#include <core/IO/Log.hpp>

#include <filesystem>

namespace core {

std::optional<std::vector<uint8_t>> readFile(std::string const& path) {
    std::error_code error;
    size_t const file_size = std::filesystem::file_size(path, error);
    if (error) {
        CORE_ERROR("Failed to get size of file {}: {}", path, error.message());
        return std::nullopt;
    }

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

    size_t const bytes_read = std::fread(dst.data(), 1, dst.size(), file);
    bool const success = bytes_read == dst.size();
    if (std::fclose(file) != 0 || !success) {
        CORE_ERROR("Failed to read file {}", path);
        return false;
    }

    return true;
}

} // namespace core
