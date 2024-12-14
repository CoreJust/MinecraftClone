#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include "UniformVariable.hpp"

namespace core::gl {
	class Shader final {
		std::unordered_map<std::string, UniformVariable> m_uniforms;
		UID m_id = 0;

	public:
		Shader(
			std::string_view vertex, 
			std::string_view fragment, 
			std::unordered_map<std::string, UniformVariable> uniforms);
		Shader(
			std::string_view vertex, 
			std::string_view geometry, 
			std::string_view fragment, 
			std::unordered_map<std::string, UniformVariable> uniforms);
		~Shader();

		void bind() const;
		void unbind() const;

		UniformVariable operator[](const std::string& name) const;
	};
}
