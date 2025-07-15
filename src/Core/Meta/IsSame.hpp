#pragma once

namespace core {
	template<class, class>
	constexpr bool IsSameV = false;
	template<class T>
	constexpr bool IsSameV<T, T> = true;
	template<class U, class V>
	concept IsSame = IsSameV<U, V>;
} // namespace core
