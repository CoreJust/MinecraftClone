#include "SolidShader.h"
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	core::gl::Shader buildSolidShader() {
		return core::gl::ShaderBuilder("solid_shader")
			.addUniform("sect_pos", { core::gl::Type::Float32x3 })
			.addUniform("projView_matrix", { core::gl::Type::Float32x4x4 })
			.buildFromFiles("solid_vertex", "basic_fragment");
	}
}

SolidShader::SolidShader() : m_shader(buildSolidShader()) { }

void SolidShader::bind() const {
	m_shader.bind();
}

core::gl::UniformVariable SolidShader::projViewMat() const {
	return m_shader["projView_matrix"];
}

core::gl::UniformVariable SolidShader::chunkSectionPos() const {
	return m_shader["sect_pos"];
}
