#pragma once
#include <Core/GL/Shader.hpp>

class HitBoxShader final {
	core::gl::Shader m_shader;

public:
	HitBoxShader();

	void bind() const;
	core::gl::UniformVariable projViewMat() const;
	core::gl::UniformVariable modelMat() const;
};