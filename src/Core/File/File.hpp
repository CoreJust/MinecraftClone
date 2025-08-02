#pragma once
#include <Core/Container/DynArray.hpp>
#include <Core/Container/StringView.hpp>

namespace core {
	class File {
	protected:
		struct FileHandle {
			StringView path;
			usize leftSize;
			usize fileSize;
			void* handle;

			~FileHandle();

			bool readTo(RawMemory mem);
			DynArray<byte> readAll();

			bool seek(usize pos);
		};

	protected:
		StringView m_path;

	public:
		explicit File(StringView path) noexcept;
		
		PURE StringView path() const noexcept { return m_path; }

	protected:
		FileHandle open();
	};
} // namespace core
