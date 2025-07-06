#pragma once

namespace core::meta {
	template<class T>
	struct UnRefTrait { using Type = T; };
	template<class T>
	struct UnRefTrait<T&> { using Type = T; };
	template<class T>
	struct UnRefTrait<T&&> { using Type = T; };
	template<class T>
	using UnRef = UnRefTrait<T>::Type;
} // namespace core::meta
