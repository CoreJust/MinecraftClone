#include "Shader.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <GL/glew.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>

namespace {
	core::gl::UID compileShader(std::string_view shader_source, GLenum shaderType) {
		static GLchar infoLog[512];

		GLint success;
		GLuint result = glCreateShader(shaderType);
		const GLchar* const data = shader_source.data();
		glShaderSource(result, 1, &data, nullptr);
		glCompileShader(result);
		glGetShaderiv(result, GL_COMPILE_STATUS, &success);
		if (!success) {
			glGetShaderInfoLog(result, 512, nullptr, infoLog);
			core::io::error("Failed to compile shader: {}", infoLog);
			return 0;
		}

		return result;
	}

	core::gl::UID compileProgram(GLuint vertex, GLuint geometry, GLuint fragment) {
		static GLchar infoLog[512];

		GLint success;
		core::gl::UID result = glCreateProgram();
		glAttachShader(result, vertex);
		if (geometry)
			glAttachShader(result, geometry);
		glAttachShader(result, fragment);
		glLinkProgram(result);

		glGetProgramiv(result, GL_LINK_STATUS, &success);
		if (!success) {
			glGetProgramInfoLog(result, 512, NULL, infoLog);
			core::io::error("Failed to compile shader program: {}", infoLog);
		}
		return result;
	}

	core::gl::SID getVariableLocation(core::gl::SID shaderId, std::string_view name) {
		GLint location = glGetUniformLocation(shaderId, name.data());
		ASSERT(location > -1, "No such uniform in Shader: " + std::string(name));
		return location;
	}

	void loadUniformLocations(core::gl::SID shaderId, std::unordered_map<std::string, core::gl::UniformVariable>& uniforms) {
		for (auto& [name, var] : uniforms) {
			ASSERT(core::gl::isRepresentableInGLSL(var.type), "Encountered a type not representable in GLSL");
			var.location = getVariableLocation(shaderId, name);
		}
	}
}

core::gl::Shader::Shader(
	std::string_view vertex, 
	std::string_view fragment, 
	std::unordered_map<std::string, UniformVariable> uniforms)
	: m_uniforms(std::move(uniforms)) {
	GLuint vertexID = compileShader(vertex, GL_VERTEX_SHADER);
	GLuint fragmentID = compileShader(fragment, GL_FRAGMENT_SHADER);
	m_id = compileProgram(vertexID, 0, fragmentID);

	glDeleteShader(vertexID);
	glDeleteShader(fragmentID);

	loadUniformLocations(m_id, m_uniforms);
}

core::gl::Shader::Shader(
	std::string_view vertex, 
	std::string_view geometry, 
	std::string_view fragment, 
	std::unordered_map<std::string, UniformVariable> uniforms)
	: m_uniforms(std::move(uniforms)) {
	GLuint vertexID = compileShader(vertex, GL_VERTEX_SHADER);
	GLuint geometryID = compileShader(geometry, GL_GEOMETRY_SHADER);
	GLuint fragmentID = compileShader(fragment, GL_FRAGMENT_SHADER);
	m_id = compileProgram(vertexID, geometryID, fragmentID);

	glDeleteShader(vertexID);
	glDeleteShader(geometryID);
	glDeleteShader(fragmentID);

	loadUniformLocations(m_id, m_uniforms);
}

core::gl::Shader::~Shader() {
	if (m_id) {
		glDeleteProgram(m_id);
		m_id = 0;
	}
}

void core::gl::Shader::bind() const {
	ASSERT(m_id, "Shader must be initialized before binding");
	glUseProgram(m_id);
}

void core::gl::Shader::unbind() const {
	glUseProgram(0);
}

core::gl::UniformVariable core::gl::Shader::operator[](const std::string& name) const {
	return m_uniforms.at(name);
}
