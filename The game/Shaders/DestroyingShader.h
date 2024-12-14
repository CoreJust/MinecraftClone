#pragma once
#include <Core/GL/Shader.hpp>

class DestroyingShader final {
	core::gl::Shader m_shader;

public:
	DestroyingShader();

	void bind() const;
	core::gl::UniformVariable projViewMat() const;
};
