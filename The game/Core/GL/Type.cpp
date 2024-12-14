#include "Type.hpp"
#include <type_traits>
#include <GL/glew.h>

GLuint core::gl::toRawGL(Type type) noexcept {
	constexpr GLuint GL_RAW_TYPES[] = {
		GL_HALF_FLOAT, GL_FLOAT, GL_DOUBLE,
		GL_UNSIGNED_BYTE, GL_BYTE,
		GL_UNSIGNED_SHORT, GL_SHORT,
		GL_UNSIGNED_INT, GL_INT,
		GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4,
		GL_FLOAT_MAT2, GL_FLOAT_MAT3, GL_FLOAT_MAT4,
	};

	return GL_RAW_TYPES[static_cast<uint8_t>(type)];
}

char const* core::gl::toGLSLTypeName(Type type) noexcept {
	constexpr char const* GLSL_TYPE_NAMES[] = {
		"", "float", "double",
		"", "",
		"", "",
		"uint", "int",
		"vec2", "vec3", "vec4",
		"mat2", "mat3", "mat4",
	};

	return GLSL_TYPE_NAMES[static_cast<uint8_t>(type)];
}

bool core::gl::isRepresentableInGLSL(Type type) noexcept {
	constexpr bool GLSL_REPRESENTABLE_TYPES[] = {
		false, true, true,
		false, false,
		false, false,
		true, true,
		true, true, true,
		true, true, true,
	};

	return GLSL_REPRESENTABLE_TYPES[static_cast<uint8_t>(type)];
}

bool core::gl::isFloat(Type type) noexcept {
	constexpr bool GL_IS_FLOAT[] = {
		true, true, true,
		false, false,
		false, false,
		false, false,
		true, true, true,
		true, true, true,
	};

	return GL_IS_FLOAT[static_cast<uint8_t>(type)];
}

bool core::gl::isScalar(Type type) noexcept {
	return static_cast<uint8_t>(type) < static_cast<uint8_t>(Type::Float32x2);
}

bool core::gl::isVector(Type type) noexcept {
	return static_cast<uint8_t>(type) >= static_cast<uint8_t>(Type::Float32x2)
		&& static_cast<uint8_t>(type) < static_cast<uint8_t>(Type::Float32x2x2);
}

bool core::gl::isMatrix(Type type) noexcept {
	return static_cast<uint8_t>(type) >= static_cast<uint8_t>(Type::Float32x2x2);
}

size_t core::gl::elementsCount(Type type) noexcept {
	constexpr size_t GL_ELEMENT_COUNTS[] = {
		1, 1, 1,
		1, 1,
		1, 1,
		1, 1,
		2, 3, 4,
		4, 9, 16
	};

	return GL_ELEMENT_COUNTS[static_cast<uint8_t>(type)];
}

size_t core::gl::typeSize(Type type) noexcept {
	constexpr size_t GL_TYPE_SIZES[] = {
		2, 4, 8,
		1, 1,
		2, 2,
		4, 4,
		8, 12, 16,
		16, 36, 64
	};

	return GL_TYPE_SIZES[static_cast<uint8_t>(type)];
}
