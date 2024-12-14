#pragma once
#include "Shader.hpp"

namespace core::gl {
	struct Attribute final {
		AttributeType type;
		std::string_view name;
	};

	enum ArgumentKind : uint8_t {
		In,
		Out,
		InOut
	};

	struct Argument final {
		std::string_view name;
		Type type;
		ArgumentKind inout = In;
	};

	struct GLSLFunction final {
		std::string_view name;
		std::vector<Argument> args;
		std::string_view body;
	};

	class ShaderBuilder final {
		std::vector<Attribute> m_vertexAttributes;
		std::vector<Attribute> m_geometryAttributes;
		std::vector<Attribute> m_fragmentAttributes;
		std::unordered_map<std::string, UniformVariable> m_uniforms;
		std::vector<GLSLFunction> m_vertexShaderFunctions;
		std::vector<GLSLFunction> m_geometryShaderFunctions;
		std::vector<GLSLFunction> m_fragmentShaderFunctions;
		std::string_view m_vertexShaderBody;
		std::string_view m_geometryShaderBody;
		std::string_view m_fragmentShaderBody;
		std::string_view m_shaderName;

	public:
		ShaderBuilder(std::string_view name) noexcept;

		ShaderBuilder& setVertexAttributes(std::vector<Attribute> attributes) noexcept;
		ShaderBuilder& setGeometryAttributes(std::vector<Attribute> attributes) noexcept;
		ShaderBuilder& setFragmentAttributes(std::vector<Attribute> attributes) noexcept;
		ShaderBuilder& setUniforms(std::unordered_map<std::string, UniformVariable> uniforms) noexcept;

		ShaderBuilder& addVertexAttribute(Attribute attribute);
		ShaderBuilder& addGeometryAttribute(Attribute attribute);
		ShaderBuilder& addFragmentAttribute(Attribute attribute);
		ShaderBuilder& addUniform(std::string name, UniformVariable uniform);

		ShaderBuilder& addVertexShaderFunction(std::string_view name, std::vector<Argument> args, std::string_view body);
		ShaderBuilder& addGeometryShaderFunction(std::string_view name, std::vector<Argument> args, std::string_view body);
		ShaderBuilder& addFragmentShaderFunction(std::string_view name, std::vector<Argument> args, std::string_view body);

		ShaderBuilder& setVertexShaderBody(std::string_view body);
		ShaderBuilder& setGeometryShaderBody(std::string_view body);
		ShaderBuilder& setFragmentShaderBody(std::string_view body);

		Shader buildFromFiles(std::string_view vertexFileName, std::string_view fragmentFileName);
		Shader buildFromFiles(std::string_view vertexFileName, std::string_view geometryFileName, std::string_view fragmentFileName);
		Shader build();
	};
}
