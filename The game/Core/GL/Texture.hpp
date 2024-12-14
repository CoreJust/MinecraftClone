#pragma once
#include <string>
#include "Fwd.hpp"

namespace core::gl {
	class Texture final {
		UID m_id = 0;

	public:
		Texture(const std::string& file_name);
		~Texture();

		void setTextureBlock(Enum block);
		void bind() const;
		void unbind() const;
	};
}