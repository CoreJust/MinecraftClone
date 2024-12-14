#include "WaterShader.h"
#include <Core/Common/Timer.hpp>
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	core::gl::Shader buildWaterShader() {
		return core::gl::ShaderBuilder("water_shader")
			.addUniform("globalTime", { core::gl::Type::Float32 })
			.addUniform("sect_pos", { core::gl::Type::Float32x3 })
			.addUniform("projView_matrix", { core::gl::Type::Float32x4x4 })
			.buildFromFiles("water_vertex", "basic_fragment");
	}
}

WaterShader::WaterShader()
	: m_shader(buildWaterShader()) { }

void WaterShader::bind() const {
	m_shader.bind();
}

void WaterShader::loadTime() {
	m_shader["globalTime"] = static_cast<float>(core::common::Timer::global().elapsed().asSeconds());
}

core::gl::UniformVariable WaterShader::projViewMat() const {
	return m_shader["projView_matrix"];
}

core::gl::UniformVariable WaterShader::chunkSectionPos() const {
	return m_shader["sect_pos"];
}
