#pragma once
#include <Core/GL/Shader.hpp>

class EntityShader final {
	core::gl::Shader m_shader;

public:
	EntityShader();

	void bind() const;
	core::gl::UniformVariable projViewMat() const;
	core::gl::UniformVariable modelMat() const;
};
