#pragma once
#include <Core/GL/Shader.hpp>

class SolidShader final {
	core::gl::Shader m_shader;

public:
	SolidShader();

	void bind() const;
	core::gl::UniformVariable projViewMat() const;
	core::gl::UniformVariable chunkSectionPos() const;
};