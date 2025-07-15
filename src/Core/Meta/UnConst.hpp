#pragma once

namespace core {
	template<class T>
	struct UnConstImpl { using Type = T; };
	template<class T>
	struct UnConstImpl<const T> { using Type = T; };
	template<class T>
	using UnConst = UnConstImpl<T>::Type;
} // namespace core
