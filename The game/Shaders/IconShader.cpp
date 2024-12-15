#include "IconShader.h"
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	core::gl::Shader buildIconShader() {
		return core::gl::ShaderBuilder("icon_shader")
			.addUniform("matrix", { core::gl::Type::Float32x4x4 })
			.addUniform("pos", { core::gl::Type::Float32x2 })
			.buildFromFiles("icon_vertex", "basic_fragment");
	}
}

IconShader::IconShader() : m_shader(buildIconShader()) { }

void IconShader::bind() const {
	m_shader.bind();
}

core::gl::UniformVariable IconShader::matrix() const {
	return m_shader["matrix"];
}

core::gl::UniformVariable IconShader::pos() const {
	return m_shader["pos"];
}
