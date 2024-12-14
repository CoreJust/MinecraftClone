#include "UniformVariable.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <GL/glew.h>
#include <Core/Common/Assert.hpp>

template<> void core::gl::UniformVariable::operator=<int32_t>(const int32_t& item) const {
	switch (type) {
		case Type::Float32:
			glUniform1f(location, item);
			break;
		case Type::UInt32:
			ASSERT(item >= 0, "Cannot assign a negative number to an unsigned uniform");
			glUniform1ui(location, item);
			break;
		case Type::Int32:
			glUniform1i(location, item);
			break;
	default:
		ASSERT(false, "Cannot assign a number to a non-numeric uniform");
		break;
	}
}

template<> void core::gl::UniformVariable::operator=<uint32_t>(const uint32_t& item) const {
	switch (type) {
		case Type::Float32:
			glUniform1f(location, item);
			break;
		case Type::UInt32:
			glUniform1ui(location, item);
			break;
		case Type::Int32:
			ASSERT(item <= INT32_MAX, "Value is out of int32 range: cannot assign to a uniform");
			glUniform1i(location, item);
			break;
	default:
		ASSERT(false, "Cannot assign a number to a non-numeric uniform");
		break;
	}
}

template<> void core::gl::UniformVariable::operator=<float>(const float& item) const {
	ASSERT(type == Type::Float32, "Cannot assign a FP to a non-FP uniform");
	glUniform1f(location, item);
}

template<> void core::gl::UniformVariable::operator=<glm::vec2>(const glm::vec2& item) const {
	ASSERT(type == Type::Float32x2, "Cannot assign a vec2 to a uniform of another type");
	glUniform2f(location, item.x, item.y);
}

template<> void core::gl::UniformVariable::operator=<glm::vec3>(const glm::vec3& item) const {
	ASSERT(type == Type::Float32x3, "Cannot assign a vec3 to a uniform of another type");
	glUniform3f(location, item.x, item.y, item.z);
}

template<> void core::gl::UniformVariable::operator=<glm::vec4>(const glm::vec4& item) const {
	ASSERT(type == Type::Float32x4, "Cannot assign a vec4 to a uniform of another type");
	glUniform4f(location, item.x, item.y, item.z, item.w);
}

template<> void core::gl::UniformVariable::operator=<glm::mat2>(const glm::mat2& item) const {
	ASSERT(type == Type::Float32x2x2, "Cannot assign a mat2 to a uniform of another type");
	glUniformMatrix2fv(location, 1, GL_FALSE, glm::value_ptr(item));
}

template<> void core::gl::UniformVariable::operator=<glm::mat3>(const glm::mat3& item) const {
	ASSERT(type == Type::Float32x3x3, "Cannot assign a mat3 to a uniform of another type");
	glUniformMatrix3fv(location, 1, GL_FALSE, glm::value_ptr(item));
}

template<> void core::gl::UniformVariable::operator=<glm::mat4>(const glm::mat4& item) const {
	ASSERT(type == Type::Float32x4x4, "Cannot assign a mat4 to a uniform of another type");
	glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(item));
}
