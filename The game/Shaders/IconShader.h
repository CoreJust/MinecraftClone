#pragma once
#include <Core/GL/Shader.hpp>

class IconShader final {
	core::gl::Shader m_shader;

public:
	IconShader();

	void bind() const;
	core::gl::UniformVariable matrix() const;
	core::gl::UniformVariable pos() const;
};