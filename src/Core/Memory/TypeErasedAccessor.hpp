#pragma once
#include <utility>

namespace core::memory {
	template<class T>
	class TypeErasedAccessor final {
		constexpr static inline size_t ObjectSize = sizeof(T);
		constexpr static inline bool IsSmallObject = ObjectSize <= sizeof(void*);

		using SmallObject = union {
			void* ptr;
			T object;
		};

	public:
		static void* make(auto&&... args) {
			if constexpr (IsSmallObject) {
				SmallObject result { nullptr };
				new(&result.object) T(std::forward<decltype(args)>(args)...);
				return result.ptr;
			} else {
				return reinterpret_cast<void*>(new T(std::forward<decltype(args)>(args)...));
			}
		}

		static void destroy(void* ptr) {
			if constexpr (IsSmallObject) {
				SmallObject result { ptr };
				result.object.~T();
			} else {
				delete reinterpret_cast<T*>(ptr);
			}
		}

		static T& get(void*& ptr) {
			if constexpr (IsSmallObject) {
				return *reinterpret_cast<T*>(&ptr);
			} else {
				return *reinterpret_cast<T*>(ptr);
			}
		}

		static T const& get(void* const& ptr) {
			if constexpr (IsSmallObject) {
				return *reinterpret_cast<T const*>(&ptr);
			} else {
				return *reinterpret_cast<T const*>(ptr);
			}
		}
	};
} // namespace core::memory
