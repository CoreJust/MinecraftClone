#include "DestroyingShader.h"
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	core::gl::Shader buildDestroyingShader() {
		return core::gl::ShaderBuilder("destroying_shader")
			.addUniform("projView_matrix", { core::gl::Type::Float32x4x4 })
			.buildFromFiles("destroying_vertex", "solid_fragment");
	}
}

DestroyingShader::DestroyingShader() : m_shader(buildDestroyingShader()) { }

void DestroyingShader::bind() const {
	m_shader.bind();
}

core::gl::UniformVariable DestroyingShader::projViewMat() const {
	return m_shader["projView_matrix"];
}
