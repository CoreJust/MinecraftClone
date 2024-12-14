#include "ShaderBuilder.hpp"
#include <format>
#include <Core/Common/Assert.hpp>
#include <Core/IO/File.hpp>
#include <Core/IO/Logger.hpp>

namespace {
	constexpr const char* SHADERS_PATH = "res/shaders/";

	std::string_view inoutToString(core::gl::ArgumentKind const kind) {
		constexpr std::string_view INOUT_NAMES[] = {
			"in", "out", "inout"
		};

		return INOUT_NAMES[kind];
	}

	void makeFunctionSignature(std::string& target, core::gl::GLSLFunction const& func) {
		target += std::format("void {}(", func.name);
		for (auto const& arg : func.args) {
			ASSERT(core::gl::isRepresentableInGLSL(arg.type), "Function argument type is not representable in GLSL");
			target += std::format("{} {} {}, ", inoutToString(arg.inout), core::gl::toGLSLTypeName(arg.type), arg.name);
		}
		if (!func.args.empty()) {
			target.pop_back();
			target.pop_back();
		}
		target += ")";
	}

	std::string buildShader(
		std::unordered_map<std::string, core::gl::UniformVariable> const& uniforms,
		std::vector<core::gl::Attribute> const& inAttributes,
		std::vector<core::gl::Attribute> const& outAttributes,
		std::vector<core::gl::GLSLFunction> const& functions,
		std::string_view mainBody
	) {
		std::string result = "#version 460 core\n\n";
		if (!uniforms.empty()) {
			for (auto const& [name, var] : uniforms)
				result += std::format("uniform {} {};\n", core::gl::toGLSLTypeName(var.type), name);
			result += '\n';
		}

		for (auto const& inAttr : inAttributes) {
			ASSERT(core::gl::isRepresentableInGLSL(inAttr.type.type), "In attribute type is not representable in GLSL");
			result += std::format("in {} {};", core::gl::toGLSLTypeName(inAttr.type.type), inAttr.name);
		}
		result += '\n';

		for (auto const& outAttr : outAttributes) {
			ASSERT(core::gl::isRepresentableInGLSL(outAttr.type.type), "Out attribute type is not representable in GLSL");
			result += std::format("out {} {};", core::gl::toGLSLTypeName(outAttr.type.type), outAttr.name);
		}
		result += '\n';

		if (!functions.empty()) {
			for (auto const& func : functions) {
				makeFunctionSignature(result, func);
				result += ";\n";
			}
			result += "\n\n";

			for (auto const& func : functions) {
				makeFunctionSignature(result, func);
				result += " {\n";
				result += func.body;
				result += "\n}\n\n";
			}
		}

		result += "void main() {\n";
		result += mainBody;
		result += "\n}\n";
		return result;
	}
}

core::gl::ShaderBuilder::ShaderBuilder(std::string_view name) noexcept
	: m_shaderName(name) { }

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setVertexAttributes(std::vector<Attribute> attributes) noexcept {
	m_vertexAttributes = std::move(attributes);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setGeometryAttributes(std::vector<Attribute> attributes) noexcept {
	m_geometryAttributes = std::move(attributes);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setFragmentAttributes(std::vector<Attribute> attributes) noexcept {
	m_fragmentAttributes = std::move(attributes);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setUniforms(std::unordered_map<std::string, UniformVariable> uniforms) noexcept {
	m_uniforms = std::move(uniforms);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addVertexAttribute(Attribute attribute) {
	m_vertexAttributes.emplace_back(std::move(attribute));
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addGeometryAttribute(Attribute attribute) {
	m_geometryAttributes.emplace_back(std::move(attribute));
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addFragmentAttribute(Attribute attribute) {
	m_fragmentAttributes.emplace_back(std::move(attribute));
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addUniform(std::string name, UniformVariable uniform) {
	m_uniforms.emplace(std::make_pair(std::move(name), std::move(uniform)));
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addVertexShaderFunction(std::string_view name, std::vector<Argument> args, std::string_view body) {
	m_vertexShaderFunctions.emplace_back(name, std::move(args), body);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addGeometryShaderFunction(std::string_view name, std::vector<Argument> args, std::string_view body) {
	m_geometryShaderFunctions.emplace_back(name, std::move(args), body);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::addFragmentShaderFunction(std::string_view name, std::vector<Argument> args, std::string_view body) {
	m_fragmentShaderFunctions.emplace_back(name, std::move(args), body);
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setVertexShaderBody(std::string_view body) {
	m_vertexShaderBody = body;
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setGeometryShaderBody(std::string_view body) {
	m_geometryShaderBody = body;
	return *this;
}

core::gl::ShaderBuilder& core::gl::ShaderBuilder::setFragmentShaderBody(std::string_view body) {
	m_fragmentShaderBody = body;
	return *this;
}

core::gl::Shader core::gl::ShaderBuilder::buildFromFiles(std::string_view vertexFileName, std::string_view fragmentFileName) {
	std::string const vertexShader = io::readFile(SHADERS_PATH + std::string(vertexFileName) + ".glsl");
	std::string const fragmentShader = io::readFile(SHADERS_PATH + std::string(fragmentFileName) + ".glsl");
	return Shader(vertexShader, fragmentShader, std::move(m_uniforms));
}

core::gl::Shader core::gl::ShaderBuilder::buildFromFiles(std::string_view vertexFileName, std::string_view geometryFileName, std::string_view fragmentFileName) {
	std::string const vertexShader = io::readFile(SHADERS_PATH + std::string(vertexFileName) + ".glsl");
	std::string const geometryShader = io::readFile(SHADERS_PATH + std::string(geometryFileName) + ".glsl");
	std::string const fragmentShader = io::readFile(SHADERS_PATH + std::string(fragmentFileName) + ".glsl");
	return Shader(vertexShader, geometryShader, fragmentShader, std::move(m_uniforms));
}

core::gl::Shader core::gl::ShaderBuilder::build() {
	ASSERT(!m_vertexShaderBody.empty(), "Vertex shader body is required for a shader program");
	ASSERT(!m_fragmentShaderBody.empty(), "Fragment shader body is required for a shader program");

	if (!m_geometryShaderBody.empty() || !m_geometryAttributes.empty() || !m_geometryShaderFunctions.empty()) {
		ASSERT(!m_geometryShaderBody.empty(), "Geometry shader is defined, but has no body");
		std::string vertexShader = buildShader(
			m_uniforms,
			m_vertexAttributes,
			m_geometryAttributes,
			m_vertexShaderFunctions,
			m_vertexShaderBody);
		std::string geometryShader = buildShader(
			m_uniforms,
			m_geometryAttributes,
			m_fragmentAttributes,
			m_geometryShaderFunctions,
			m_geometryShaderBody);
		std::string fragmentShader = buildShader(
			m_uniforms,
			m_fragmentAttributes,
			{{ { Type::Float32x4 }, "color" } },
			m_fragmentShaderFunctions,
			m_fragmentShaderBody);

		io::debug(
			"Generated shader program {};\nVertex shader:\n{}\n\nGeometry shader:\n{}\n\nFragment shader:\n{}\n\n",
			m_shaderName,
			vertexShader,
			geometryShader,
			fragmentShader);
		return Shader(vertexShader, geometryShader, fragmentShader, std::move(m_uniforms));
	} else {
		std::string vertexShader = buildShader(
			m_uniforms,
			m_vertexAttributes,
			m_fragmentAttributes,
			m_vertexShaderFunctions,
			m_vertexShaderBody);
		std::string fragmentShader = buildShader(
			m_uniforms,
			m_fragmentAttributes,
			{{ { Type::Float32x4 }, "color" }},
			m_fragmentShaderFunctions,
			m_fragmentShaderBody);

		io::debug(
			"Generated shader program {};\nVertex shader:\n{}\n\nFragment shader:\n{}\n\n",
			m_shaderName,
			vertexShader,
			fragmentShader);
		return Shader(vertexShader, fragmentShader, std::move(m_uniforms));
	}
}
