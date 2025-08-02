#include "File.hpp"
#include <string_view>
#include <filesystem>
#include <Core/IO/Logger.hpp>

namespace core {
	File::FileHandle::~FileHandle() {
		if (handle) {
			std::fclose(reinterpret_cast<std::FILE*>(handle));
			handle = nullptr;
		}
	}

	bool File::FileHandle::readTo(RawMemory mem) {
		ASSERT(handle != nullptr && mem.size <= leftSize);
		
		if (mem.size != std::fread(reinterpret_cast<void*>(mem.data), 1, mem.size, reinterpret_cast<std::FILE*>(handle))) {
			error("Failed to read file {}", std::string_view { path.data(), path.size() });
			return false;
		}

		leftSize -= mem.size;
		return true;
	}

	DynArray<byte> File::FileHandle::readAll() {
		ASSERT(handle != nullptr);
		DynArray<byte> result = DynArray<byte>::uninitialized(leftSize);
		readTo(result.raw());
		return result;
	}
			
	bool File::FileHandle::seek(usize pos) {
		ASSERT(pos < fileSize);

		if (std::fseek(reinterpret_cast<std::FILE*>(handle), static_cast<long>(pos), SEEK_SET)) {
			core::error("Failed to seek position {} in file {}, file size {}", pos, path, fileSize);
			return false;
		}
		leftSize = fileSize - pos;
		return true;
	}

	File::File(StringView path) noexcept : m_path(path) { }
		
	File::FileHandle File::open() {
		std::string_view path { m_path.data(), m_path.size() };
		if (!std::filesystem::exists(path)) {
			error("No such file: {}", path);
			return {};
		}

		std::FILE* file = std::fopen(path.data(), "rb");
		if (file == nullptr) {
			error("Failed to open file {}", path);
			return {};
		}

		usize const fileSize = std::filesystem::file_size(path);
		return FileHandle {
			.path = m_path,
			.leftSize = fileSize,
			.fileSize = fileSize,
			.handle = file,
		};
	}
} // namespace core
