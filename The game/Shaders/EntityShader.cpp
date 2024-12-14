#include "EntityShader.h"
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	core::gl::Shader buildEntityShader() {
		return core::gl::ShaderBuilder("entity_shader")
			.addUniform("model_matrix", { core::gl::Type::Float32x4x4 })
			.addUniform("projView_matrix", { core::gl::Type::Float32x4x4 })
			.buildFromFiles("entity_vertex", "basic_fragment");
	}
}

EntityShader::EntityShader() : m_shader(buildEntityShader()) { }

void EntityShader::bind() const {
	m_shader.bind();
}

core::gl::UniformVariable EntityShader::projViewMat() const {
	return m_shader["projView_matrix"];
}

core::gl::UniformVariable EntityShader::modelMat() const {
	return m_shader["model_matrix"];
}
