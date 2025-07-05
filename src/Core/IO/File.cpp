#include "File.hpp"
#include <filesystem>
#include "Logger.hpp"

namespace core::io {
	std::string readFile(std::string_view path) {
		if (!std::filesystem::exists(path)) {
			error("No such file: {}", path);
			return "";
		}

		size_t const fileSize = std::filesystem::file_size(path);
		std::string result(fileSize, 0);

		std::FILE* file = std::fopen(path.data(), "rb");
		if (file == nullptr) {
			error("Failed to open file {}", path);
			return "";
		}

		if (fileSize != std::fread(reinterpret_cast<void*>(&result[0]), 1, fileSize, file)) {
			error("Failed to read file {}", path);
			std::fclose(file);
			return "";
		}

		std::fclose(file);
		return result;
	}
} // namespace core::io
