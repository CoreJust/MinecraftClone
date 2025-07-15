#pragma once
#include <Core/Common/Assert.hpp>
#include "TypeErasedAccessor.hpp"
#include <typeinfo>

namespace core {
	class TypeErasedObject final {
		using Deleter = void(*)(void* ptr);

		void* m_data = nullptr;
		Deleter m_deleter = nullptr;
		usize m_typeHashCode = 0;

	private:
		TypeErasedObject(void* data, Deleter deleter, usize typeHashCode) noexcept;

	public:
		TypeErasedObject() = default;
		TypeErasedObject(TypeErasedObject&&) noexcept;
		TypeErasedObject(const TypeErasedObject&) = delete;
		TypeErasedObject& operator=(TypeErasedObject&&) noexcept;
		TypeErasedObject& operator=(const TypeErasedObject&) = delete;
		~TypeErasedObject();

		bool hasValue() const noexcept;

		template<class T>
		static TypeErasedObject make(auto&&... args) {
			return TypeErasedObject(
				TypeErasedAccessor<T>::make(std::forward<decltype(args)>(args)...),
				Deleter([](void* ptr) { TypeErasedAccessor<T>::destroy(ptr); }),
				typeid(T).hash_code()
			);
		}

		template<class T>
		T& get() {
			ASSERT(typeid(T).hash_code() == m_typeHashCode);
			return TypeErasedAccessor<T>::get(m_data);
		}

		template<class T>
		T const& get() const {
			ASSERT(typeid(T).hash_code() == m_typeHashCode);
			return TypeErasedAccessor<T const>::get(m_data);
		}
	};
} // namespace core
