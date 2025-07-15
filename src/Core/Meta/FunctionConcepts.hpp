#pragma once
#include "IsSame.hpp"

namespace core {
	template<class T>
	concept IsCallable = requires (T t) { { t() }; };

	template<class T>
	concept IsCallablePtr = requires (T t) { { (*t)() }; };

	template<class T, class Result, class... Args>
	concept Callable = requires (T t, Args&&... args) { { t(static_cast<Args&&>(args)...) } -> IsSame<Result>; };
} // namespace core
