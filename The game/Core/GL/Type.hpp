#pragma once
#include "Fwd.hpp"

namespace core::gl {
	enum class Type : uint8_t {
		Float16,
		Float32,
		Float64,
		UInt8,
		Int8,
		UInt16,
		Int16,
		UInt32,
		Int32,
		Float32x2,
		Float32x3,
		Float32x4,
		Float32x2x2,
		Float32x3x3,
		Float32x4x4,
	};

	struct AttributeType final {
		Type type;
		bool isNormalized = false;
	};

	UInt toRawGL(Type type) noexcept;
	char const* toGLSLTypeName(Type type) noexcept;
	bool isRepresentableInGLSL(Type type) noexcept;
	bool isFloat(Type type) noexcept;
	bool isScalar(Type type) noexcept;
	bool isVector(Type type) noexcept;
	bool isMatrix(Type type) noexcept;
	size_t elementsCount(Type type) noexcept;
	size_t typeSize(Type type) noexcept;
}
