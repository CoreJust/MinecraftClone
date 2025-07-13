#pragma once

namespace core::meta {
	template<class T>
	struct UnRefImpl { using Type = T; };
	template<class T>
	struct UnRefImpl<T&> { using Type = T; };
	template<class T>
	struct UnRefImpl<T&&> { using Type = T; };
	template<class T>
	using UnRef = UnRefImpl<T>::Type;
} // namespace core::meta
