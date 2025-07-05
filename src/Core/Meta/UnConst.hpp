#pragma once
#include <concepts>

namespace core::meta {
	template<class T>
	struct UnConstTrait { using Type = T; };
	template<class T>
	struct UnConstTrait<const T> { using Type = T; };
	template<class T>
	using UnConst = UnConstTrait<T>::Type;
} // namespace core::meta
