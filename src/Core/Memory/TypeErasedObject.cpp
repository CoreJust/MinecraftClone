#include "TypeErasedObject.hpp"

namespace core {
	TypeErasedObject::TypeErasedObject(void* data, Deleter deleter, usize typeHashCode) noexcept
		: m_data(data), m_deleter(deleter), m_typeHashCode(typeHashCode) { }

	TypeErasedObject::TypeErasedObject(TypeErasedObject&& other) noexcept
		: m_data(other.m_data), m_deleter(other.m_deleter), m_typeHashCode(other.m_typeHashCode) {
		other.m_data = nullptr;
		other.m_deleter = nullptr;
		other.m_typeHashCode = 0;
	}

	TypeErasedObject& TypeErasedObject::operator=(TypeErasedObject&& other) &noexcept {
        if (this != &other) {
			m_data = other.m_data;
			m_deleter = other.m_deleter;
			m_typeHashCode = other.m_typeHashCode;
			other.m_data = nullptr;
			other.m_deleter = nullptr;
			other.m_typeHashCode = 0;
		}
		return *this;
	}

	TypeErasedObject::~TypeErasedObject() {
		if (m_data) {
			m_deleter(m_data);
			m_data = nullptr;
		}
	}

	bool TypeErasedObject::hasValue() const noexcept {
		return m_data != nullptr;
	}
} // namespace core
