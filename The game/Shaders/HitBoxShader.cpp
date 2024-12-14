#include "HitBoxShader.h"
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	core::gl::Shader buildHitBoxShader() {
		return core::gl::ShaderBuilder("hitbox_shader")
			.addUniform("model_matrix", { core::gl::Type::Float32x4x4 })
			.addUniform("projView_matrix", { core::gl::Type::Float32x4x4 })
			.buildFromFiles("hitbox_vertex", "hitbox_fragment");
	}
}

HitBoxShader::HitBoxShader() : m_shader(buildHitBoxShader()) { }

void HitBoxShader::bind() const {
	m_shader.bind();
}

core::gl::UniformVariable HitBoxShader::projViewMat() const {
	return m_shader["projView_matrix"];
}

core::gl::UniformVariable HitBoxShader::modelMat() const {
	return m_shader["model_matrix"];
}
