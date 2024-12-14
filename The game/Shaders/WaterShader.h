#pragma once
#include <Core/GL/Shader.hpp>

class WaterShader final {
	core::gl::Shader m_shader;

public:
	WaterShader();

	void bind() const;
	void loadTime();
	core::gl::UniformVariable projViewMat() const;
	core::gl::UniformVariable chunkSectionPos() const;
};